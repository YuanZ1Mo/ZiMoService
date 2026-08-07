#include "module_file_hub.h"

#include "zm_net_http.h"
#include "service_define.h"
#include "zm_logger.h"
#include "zm_util_str.h"
#include "zm_util_sys.h"

#include <windows.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <event2/buffer.h>

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <iomanip>
#include <share.h>
#include <sstream>

// ============================================================================
// 构造 / 析构
// ============================================================================

FileHubModule::FileHubModule()
{
	// 自行推导 www 根目录(exe 在 $(SolutionDir)$(Configuration)\ 下,www 与 exe 同目录)
	char exePath[MAX_PATH];
	ZmSystem::GetModuleDir(exePath, MAX_PATH);
	m_wwwRoot = std::string(exePath) + "\\www";

	std::wstring hubRoot = ZmString::UTF8_To_Unicode(GetHubRoot());
	CreateDirectoryW(hubRoot.c_str(), nullptr);
}

FileHubModule::~FileHubModule()
{
}

// ============================================================================
// 路径工具
// ============================================================================

std::string FileHubModule::GetHubRoot() const
{
	std::string root = m_wwwRoot;
	if (!root.empty() && root.back() != '\\')
		root += '\\';
	root += ZM_FILE_HUB_ROOT;
	return root;
}

bool FileHubModule::NormalizeHubPath(const std::string& relativePath, std::string& absPath)
{
	std::wstring hubRoot = ZmString::UTF8_To_Unicode(GetHubRoot());

	// 拼接路径
	std::string rawPath = GetHubRoot();
	if (!relativePath.empty())
	{
		rawPath += "\\";
		rawPath += relativePath;
	}
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	// 用 Wide API 规范化
	std::wstring raw = ZmString::UTF8_To_Unicode(rawPath);
	WCHAR normalized[MAX_PATH];
	if (!GetFullPathNameW(raw.c_str(), MAX_PATH, normalized, nullptr))
		return false;
	absPath = ZmString::Unicode_To_UTF8(normalized);

	// 规范化 hubRoot
	WCHAR normRoot[MAX_PATH];
	if (!GetFullPathNameW(hubRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return false;
	std::string normRootStr = ZmString::Unicode_To_UTF8(normRoot);

	// 防路径穿越
	if (absPath.size() < normRootStr.size() ||
	    _strnicmp(absPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return false;

	return true;
}

// ============================================================================
// 用户配置读写
// ============================================================================

bool FileHubModule::ReadUserConfig(const std::string& dirAbsPath, ZMJSON& config)
{
	std::string configPath = dirAbsPath + "\\.userConfig";
	std::wstring wpath = ZmString::UTF8_To_Unicode(configPath);
	std::ifstream file(wpath.c_str());
	if (!file.is_open())
		return false;
	try
	{
		file >> config;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool FileHubModule::WriteUserConfig(const std::string& dirAbsPath,
	const ZMJSON& config)
{
	std::string configPath = dirAbsPath + "\\.userConfig";
	std::wstring wpath = ZmString::UTF8_To_Unicode(configPath);
	std::ofstream file(wpath.c_str(), std::ios::trunc);
	if (!file.is_open())
		return false;
	file << config.dump(2);
	return true;
}

// ============================================================================
// 密码管理
// ============================================================================

std::string FileHubModule::HashPassword(const std::string& password)
{
	unsigned char result[EVP_MAX_MD_SIZE];
	unsigned int resultLen = 0;

	HMAC(EVP_sha256(),
		ZM_FILE_HUB_HMAC_KEY, (int)strlen(ZM_FILE_HUB_HMAC_KEY),
		(const unsigned char*)password.c_str(), password.size(),
		result, &resultLen);

	std::ostringstream hex;
	hex << std::hex << std::setfill('0');
	for (unsigned int i = 0; i < resultLen; i++)
		hex << std::setw(2) << (int)result[i];
	return hex.str();
}

bool FileHubModule::VerifyPassword(const ZMJSON& config,
	const std::string& password)
{
	if (!config.contains("user_info"))
		return true;

	auto& info = config["user_info"];
	if (!info.contains("password_hash"))
		return true;

	std::string storedHash = info["password_hash"];
	std::string inputHash = HashPassword(password);
	return storedHash == inputHash;
}

// ============================================================================
// JRPC 方法
// ============================================================================

ZMJSON FileHubModule::ListFiles(const std::string& relativePath)
{
	ZMJSON result;
	result["ok"] = true;
	result["files"] = ZMJSON::array();

	std::string absPath;
	if (!NormalizeHubPath(relativePath, absPath))
	{
		result["ok"] = false;
		result["error"] = "路径无效";
		return result;
	}

	// 枚举目录内容（Wide API）
	std::wstring searchPattern = ZmString::UTF8_To_Unicode(absPath) + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return result;

	std::vector<ZMJSON> folders;
	std::vector<ZMJSON> files;

	do
	{
		std::string name = ZmString::Unicode_To_UTF8(fd.cFileName);
		// 跳过 . 和 .. 以及隐藏文件
		if (name == "." || name == ".." || name[0] == '.')
			continue;

		ZMJSON item;
		item["name"] = name;

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			item["type"] = "folder";
			item["size"] = 0;

			// 检查是否有子项
			std::wstring subPattern = ZmString::UTF8_To_Unicode(absPath) + L"\\" + fd.cFileName + L"\\*";
			WIN32_FIND_DATAW subFd;
			HANDLE hSub = FindFirstFileW(subPattern.c_str(), &subFd);
			item["hasChild"] = (hSub != INVALID_HANDLE_VALUE);
			if (hSub != INVALID_HANDLE_VALUE)
				FindClose(hSub);

			// 检查是否设了密码
			std::string dirAbsPath = absPath + "\\" + name;
			ZMJSON cfg;
			bool hasPwd = false;
			if (ReadUserConfig(dirAbsPath, cfg))
			{
				if (cfg.contains("user_info") &&
				    cfg["user_info"].contains("password_hash") &&
				    !cfg["user_info"]["password_hash"].get<std::string>().empty())
					hasPwd = true;
			}
			item["hasPassword"] = hasPwd;

			folders.push_back(std::move(item));
		}
		else
		{
			item["type"] = "file";
			ULONGLONG fileSize = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			item["size"] = (uint64_t)fileSize;
			item["hasChild"] = false;

			files.push_back(std::move(item));
		}
	} while (FindNextFileW(hFind, &fd));

	FindClose(hFind);

	auto sortByName = [](const ZMJSON& a, const ZMJSON& b) {
		return a["name"].get<std::string>() < b["name"].get<std::string>();
	};
	std::sort(folders.begin(), folders.end(), sortByName);
	std::sort(files.begin(), files.end(), sortByName);

	for (auto& f : folders)
		result["files"].push_back(std::move(f));
	for (auto& f : files)
		result["files"].push_back(std::move(f));

	return result;
}

ZMJSON FileHubModule::SearchFiles(const std::string& keyword)
{
	ZMJSON result;
	result["ok"] = true;
	result["results"] = ZMJSON::array();

	if (keyword.empty())
		return result;

	std::string hubRoot = GetHubRoot();
	std::vector<std::string> results;
	SearchRecursive(hubRoot, "", keyword, results);

	for (auto& r : results)
		result["results"].push_back(r);

	return result;
}

void FileHubModule::SearchRecursive(const std::string& absDir,
	const std::string& relativeDir, const std::string& keyword,
	std::vector<std::string>& results)
{
	std::wstring searchPattern = ZmString::UTF8_To_Unicode(absDir) + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	std::string kwLower = keyword;
	std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);

	do
	{
		std::string name = ZmString::Unicode_To_UTF8(fd.cFileName);
		if (name == "." || name == ".." || name[0] == '.')
			continue;

		std::string nameLower = name;
		std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

		if (nameLower.find(kwLower) != std::string::npos)
		{
			std::string fullPath = relativeDir.empty() ? name : relativeDir + "/" + name;
			results.push_back(fullPath);
		}

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			std::string subAbs = absDir + "\\" + name;
			std::string subRel = relativeDir.empty() ? name : relativeDir + "/" + name;
			SearchRecursive(subAbs, subRel, keyword, results);
		}
	} while (FindNextFileW(hFind, &fd));

	FindClose(hFind);
}

ZMJSON FileHubModule::CreateDir(const std::string& parentPath,
	const std::string& dirName, const std::string& username, const std::string& password)
{
	ZMJSON result;
	result["ok"] = true;

	if (dirName.empty())
	{
		result["ok"] = false;
		result["error"] = "目录名称不能为空";
		return result;
	}

	if (dirName.find_first_of("<>:\"/\\|?*") != std::string::npos)
	{
		result["ok"] = false;
		result["error"] = "目录名称含非法字符";
		return result;
	}

	// 根目录下创建文件目录时用户名必填
	if (parentPath.empty() && username.empty())
	{
		result["ok"] = false;
		result["error"] = "文件目录必须填写用户名";
		return result;
	}

	// 用户名和密码仅允许字母数字
	auto isAlnum = [](const std::string& s) -> bool {
		for (char c : s) {
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
				return false;
		}
		return true;
	};
	if (!username.empty() && !isAlnum(username))
	{
		result["ok"] = false;
		result["error"] = "用户名仅允许字母和数字";
		return result;
	}
	if (!password.empty() && !isAlnum(password))
	{
		result["ok"] = false;
		result["error"] = "密码仅允许字母和数字";
		return result;
	}

	std::string parentAbs;
	if (!NormalizeHubPath(parentPath, parentAbs))
	{
		result["ok"] = false;
		result["error"] = "父路径无效";
		return result;
	}

	std::string newDirPath = parentAbs + "\\" + dirName;
	std::wstring wNewDir = ZmString::UTF8_To_Unicode(newDirPath);
	if (!CreateDirectoryW(wNewDir.c_str(), nullptr))
	{
		DWORD err = GetLastError();
		if (err == ERROR_ALREADY_EXISTS)
		{
			result["ok"] = false;
			result["error"] = "目录已存在";
		}
		else
		{
			result["ok"] = false;
			result["error"] = "创建目录失败（错误码: " + std::to_string(err) + "）";
		}
		return result;
	}

	if (!username.empty() || !password.empty())
	{
		ZMJSON config;
		config["user_info"]["username"] = username;
		config["user_info"]["password_hash"] = password.empty() ?
			"" : HashPassword(password);
		config["user_setting"] = ZMJSON::object();

		if (!WriteUserConfig(newDirPath, config))
		{
			DEFAULT_LOG_ERROR("写入 .userConfig 失败: {}", newDirPath);
		}
	}

	DEFAULT_LOG_INFO("文件中心目录已创建: {}", newDirPath);
	return result;
}

/** @brief 递归删除整个目录（包括所有文件和子目录） */
static bool DeleteDirRecursive(const std::wstring& dirPath)
{
	std::wstring searchPattern = dirPath + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return RemoveDirectoryW(dirPath.c_str()) != 0;

	do
	{
		std::wstring name(fd.cFileName);
		if (name == L"." || name == L"..")
			continue;

		std::wstring fullPath = dirPath + L"\\" + name;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			DeleteDirRecursive(fullPath);
		else
			DeleteFileW(fullPath.c_str());
	} while (FindNextFileW(hFind, &fd));

	FindClose(hFind);
	return RemoveDirectoryW(dirPath.c_str()) != 0;
}

ZMJSON FileHubModule::DeleteItem(const std::string& relativePath,
	const std::string& username, const std::string& password)
{
	ZMJSON result;
	result["ok"] = true;

	if (relativePath.empty())
	{
		result["ok"] = false;
		result["error"] = "路径不能为空";
		return result;
	}

	std::string absPath;
	if (!NormalizeHubPath(relativePath, absPath))
	{
		result["ok"] = false;
		result["error"] = "路径无效";
		return result;
	}

	std::wstring wAbs = ZmString::UTF8_To_Unicode(absPath);
	DWORD attrs = GetFileAttributesW(wAbs.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES)
	{
		result["ok"] = false;
		result["error"] = "文件或目录不存在";
		return result;
	}

	bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

	if (isDir)
	{
		ZMJSON config;
		if (ReadUserConfig(absPath, config))
		{
			auto& info = config["user_info"];
			// 有用户名时必须校验用户名
			if (info.contains("username") && !info["username"].get<std::string>().empty())
			{
				if (info["username"] != username)
				{
					result["ok"] = false;
					result["error"] = "用户名错误";
					return result;
				}
			}
			// 有密码时必须校验密码
			if (info.contains("password_hash") && !info["password_hash"].get<std::string>().empty())
			{
				if (!VerifyPassword(config, password))
				{
					result["ok"] = false;
					result["error"] = "密码错误";
					return result;
				}
			}
		}

		// 递归删除整个目录（含所有文件和子目录）
		if (!DeleteDirRecursive(wAbs))
		{
			result["ok"] = false;
			result["error"] = "删除目录失败";
			return result;
		}
	}
	else
	{
		if (!DeleteFileW(wAbs.c_str()))
		{
			result["ok"] = false;
			result["error"] = "删除文件失败";
			return result;
		}
	}

	DEFAULT_LOG_INFO("已删除: {}", absPath);
	return result;
}

ZMJSON FileHubModule::VerifyDirPassword(const std::string& relativePath,
	const std::string& password)
{
	ZMJSON result;
	result["ok"] = true;
	result["valid"] = false;

	std::string absPath;
	if (!NormalizeHubPath(relativePath, absPath))
	{
		result["ok"] = false;
		result["error"] = "路径无效";
		return result;
	}

	// 检查目录是否实际存在
	std::wstring wAbs = ZmString::UTF8_To_Unicode(absPath);
	DWORD attrs = GetFileAttributesW(wAbs.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
	{
		result["ok"] = false;
		result["error"] = "目录不存在";
		return result;
	}

	ZMJSON config;
	if (!ReadUserConfig(absPath, config))
	{
		result["valid"] = true;
		return result;
	}

	if (!config.contains("user_info") ||
	    !config["user_info"].contains("password_hash") ||
	    config["user_info"]["password_hash"].get<std::string>().empty())
	{
		result["valid"] = true;
		return result;
	}

	result["valid"] = VerifyPassword(config, password);
	return result;
}

ZMJSON FileHubModule::ChangeDirPassword(const std::string& relativePath,
	const std::string& username, const std::string& oldPassword,
	const std::string& newPassword)
{
	ZMJSON result;
	result["ok"] = true;

	std::string absPath;
	if (!NormalizeHubPath(relativePath, absPath))
	{
		result["ok"] = false;
		result["error"] = "路径无效";
		return result;
	}

	if (username.empty())
	{
		result["ok"] = false;
		result["error"] = "用户名不能为空";
		return result;
	}

	auto isAlnum = [](const std::string& s) -> bool {
		for (char c : s) {
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
				return false;
		}
		return true;
	};
	if (!isAlnum(username))
	{
		result["ok"] = false;
		result["error"] = "用户名仅允许字母和数字";
		return result;
	}
	if (!newPassword.empty() && !isAlnum(newPassword))
	{
		result["ok"] = false;
		result["error"] = "密码仅允许字母和数字";
		return result;
	}

	ZMJSON config;

	if (ReadUserConfig(absPath, config))
	{
		// 校验用户名
		if (config.contains("user_info") &&
		    config["user_info"].contains("username") &&
		    config["user_info"]["username"] != username)
		{
			result["ok"] = false;
			result["error"] = "用户名错误";
			return result;
		}

		if (config["user_info"].contains("password_hash") &&
		    !config["user_info"]["password_hash"].get<std::string>().empty())
		{
			if (!VerifyPassword(config, oldPassword))
			{
				result["ok"] = false;
				result["error"] = "旧密码错误";
				return result;
			}
		}
	}
	else
	{
		// 目录无配置，首次设置用户名密码
	}

	config["user_info"]["username"] = username;
	config["user_info"]["password_hash"] = newPassword.empty() ?
		"" : HashPassword(newPassword);

	if (!config.contains("user_setting"))
		config["user_setting"] = ZMJSON::object();

	if (!WriteUserConfig(absPath, config))
	{
		result["ok"] = false;
		result["error"] = "写入配置文件失败";
		return result;
	}

	DEFAULT_LOG_INFO("密码已更新: {}", absPath);
	return result;
}

ZMJSON FileHubModule::BatchDelete(const ZMJSON& paths,
	const std::string& username, const std::string& password)
{
	ZMJSON result;
	result["ok"] = true;
	result["deleted"] = 0;

	if (!paths.is_array())
	{
		result["ok"] = false;
		result["error"] = "参数应为路径数组";
		return result;
	}

	for (auto& p : paths)
	{
		if (!p.is_string())
			continue;
		ZMJSON r = DeleteItem(p.get<std::string>(), username, password);
		if (r["ok"].get<bool>())
			result["deleted"] = result["deleted"].get<int>() + 1;
	}

	return result;
}

// ============================================================================
// 通用文件下载 / 上传
// ============================================================================

std::string FileHubModule::ExtractFilename(const std::string& uri)
{
	std::string path = uri;
	size_t qpos = path.find('?');
	if (qpos != std::string::npos) path = path.substr(0, qpos);
	size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos) return path.substr(slash + 1);
	return path;
}

int FileHubModule::ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
	const std::string& rangeStr, int64_t fileSize)
{
	if (rangeStr.size() < 7 || _strnicmp(rangeStr.c_str(), "bytes=", 6) != 0)
		return -1;

	std::string rangeVal = rangeStr.substr(6);
	if (rangeVal.find(',') != std::string::npos) return -1;

	size_t dashPos = rangeVal.find('-');
	if (dashPos == std::string::npos) return -1;

	int64_t start = 0, end = fileSize - 1;
	std::string startStr = rangeVal.substr(0, dashPos);
	std::string endStr = rangeVal.substr(dashPos + 1);

	if (startStr.empty() && !endStr.empty()) {
		int64_t suffixLen = std::stoll(endStr);
		if (suffixLen >= fileSize) start = 0;
		else start = fileSize - suffixLen;
		end = fileSize - 1;
	} else if (!startStr.empty() && endStr.empty()) {
		start = std::stoll(startStr);
		end = fileSize - 1;
	} else {
		start = std::stoll(startStr);
		end = std::stoll(endStr);
	}

	if (start < 0 || end >= fileSize || start > end) {
		task->SetReply(ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE, "Range Not Satisfiable");
		task->PutReplyHeader("Content-Range", ("bytes */" + std::to_string(fileSize)).c_str());
		return ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE;
	}

	int64_t rangeLength = end - start + 1;
	int fd = -1;
	if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return ZM_HTTP_STATUS_CODE_NOT_FOUND;

	task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(path));
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + ExtractFilename(path) + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");
	task->PutReplyHeader("Content-Range",
		("bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize)).c_str());
	task->SetReply(ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT, "Partial Content");

	if (task->SetReplyFile(fd, start, rangeLength) != 0) { _close(fd); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }
	return ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT;
}

int FileHubModule::SendFile(ZmHttpdTask* task, const std::string& physicalPath)
{
	int fd = -1;
	if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return ZM_HTTP_STATUS_CODE_NOT_FOUND;

	int64_t fileSize = _filelengthi64(fd);
	if (fileSize <= 0) { _close(fd); return ZM_HTTP_STATUS_CODE_NOT_FOUND; }

	const char* rangeHeader = task->GetRequestHeader("Range");
	if (rangeHeader && rangeHeader[0]) {
		_close(fd);
		int rangeResult = ServeFileWithRange(task, physicalPath, rangeHeader, fileSize);
		if (rangeResult > 0) return rangeResult;
		if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return ZM_HTTP_STATUS_CODE_NOT_FOUND;
	}

	task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(physicalPath));
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + ExtractFilename(physicalPath) + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");
	task->SetReply(200);

	if (task->SetReplyFile(fd, 0, fileSize) != 0) { _close(fd); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }
	return ZM_HTTP_STATUS_CODE_OK;
}

int FileHubModule::ReceiveFile(ZmHttpdTask* task, const std::string& physicalPath,
	const BYTE* data, size_t dlen)
{
	if (!data || dlen == 0) return ZM_HTTP_STATUS_CODE_BAD_REQUEST;

	// 确保父目录存在
	std::string dirPath = physicalPath;
	size_t lastSlash = dirPath.find_last_of("\\/");
	if (lastSlash != std::string::npos) {
		dirPath = dirPath.substr(0, lastSlash);
		std::wstring wDir = ZmString::UTF8_To_Unicode(dirPath);
		if (!CreateDirectoryW(wDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
			DEFAULT_LOG_ERROR("创建上传目录失败: {}", dirPath);
			return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;
		}
	}

	std::wstring wPath = ZmString::UTF8_To_Unicode(physicalPath);
	HANDLE hFile = CreateFileW(wPath.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		DEFAULT_LOG_ERROR("创建上传文件失败: {}", physicalPath);
		return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;
	}

	LARGE_INTEGER liSize;
	liSize.QuadPart = (LONGLONG)dlen;
	if (!SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN) || !SetEndOfFile(hFile)) {
		CloseHandle(hFile); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;
	}

	HANDLE hMapping = CreateFileMappingW(hFile, NULL, PAGE_READWRITE,
		liSize.HighPart, liSize.LowPart, NULL);
	if (!hMapping) { CloseHandle(hFile); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }

	BYTE* mappedView = (BYTE*)MapViewOfFile(hMapping, FILE_MAP_WRITE, 0, 0, dlen);
	if (!mappedView) { CloseHandle(hMapping); CloseHandle(hFile); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }

	struct evbuffer* inbuf = task->GetInputBuffer();
	if (inbuf && evbuffer_get_length(inbuf) >= dlen) {
		evbuffer_copyout(inbuf, mappedView, dlen);
		evbuffer_drain(inbuf, dlen);
	} else {
		memcpy(mappedView, data, dlen);
	}

	UnmapViewOfFile(mappedView);
	CloseHandle(hMapping);
	CloseHandle(hFile);

	DEFAULT_LOG_INFO("文件上传成功: {} ({} bytes)", physicalPath, dlen);
	return ZM_HTTP_STATUS_CODE_CREATED;
}
