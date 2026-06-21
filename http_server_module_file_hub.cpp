#include "http_server_module_file_hub.h"

#include "http_server_manager.h"
#include "zm_net_http.h"
#include "service_define.h"
#include "zm_logger.h"
#include "zm_util_str.h"

#include <windows.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

// ============================================================================
// 构造 / 析构
// ============================================================================

HttpServerModuleFileHub::HttpServerModuleFileHub(const std::string& wwwRoot)
	: m_wwwRoot(wwwRoot)
{
	std::wstring hubRoot = ZmString::UTF8_To_Unicode(GetHubRoot());
	CreateDirectoryW(hubRoot.c_str(), nullptr);
}

HttpServerModuleFileHub::~HttpServerModuleFileHub()
{
}

// ============================================================================
// 路径工具
// ============================================================================

std::string HttpServerModuleFileHub::GetHubRoot() const
{
	std::string root = m_wwwRoot;
	if (!root.empty() && root.back() != '\\')
		root += '\\';
	root += ZM_FILE_HUB_ROOT;
	return root;
}

bool HttpServerModuleFileHub::NormalizeHubPath(const std::string& relativePath, std::string& absPath)
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

bool HttpServerModuleFileHub::ReadUserConfig(const std::string& dirAbsPath, ZMJSON& config)
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

bool HttpServerModuleFileHub::WriteUserConfig(const std::string& dirAbsPath,
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

std::string HttpServerModuleFileHub::HashPassword(const std::string& password)
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

bool HttpServerModuleFileHub::VerifyPassword(const ZMJSON& config,
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
// HTTP 路由注册
// ============================================================================

/**
 * @brief 将 /filehub/download/xxx URI 映射到 www/db/filehub/xxx 物理路径
 */
static std::string MapHubPath(const std::string& wwwRoot, const std::string& uri,
	const char* prefix)
{
	size_t skip = strlen(prefix);
	std::string filePath = uri.substr(skip);
	filePath = ZmString::URLDecode(filePath);

	std::string rawPath = wwwRoot + "\\db\\filehub\\" + filePath;
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	return rawPath;
}

void HttpServerModuleFileHub::RegisterHttpRoutes(ZmHttpRouter& router,
	HttpServerManager* httpMgr)
{
	// 文件中心下载
	router.Any("/filehub/download/*", [this, httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		std::string uri(task->Uri() ? task->Uri() : "/");
		std::string physPath = MapHubPath(m_wwwRoot, uri, "/filehub/download/");
		return httpMgr->SendFile(task, physPath);
	});

	// 文件中心上传
	router.Post("/filehub/upload/*", [this, httpMgr](ZmHttpdTask* task, const BYTE* data, size_t dlen) {
		std::string uri(task->Uri() ? task->Uri() : "/");
		std::string physPath = MapHubPath(m_wwwRoot, uri, "/filehub/upload/");
		return httpMgr->ReceiveFile(task, physPath, data, dlen);
	});
}

// ============================================================================
// JRPC 方法
// ============================================================================

ZMJSON HttpServerModuleFileHub::ListFiles(const std::string& relativePath)
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

ZMJSON HttpServerModuleFileHub::SearchFiles(const std::string& keyword)
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

void HttpServerModuleFileHub::SearchRecursive(const std::string& absDir,
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

ZMJSON HttpServerModuleFileHub::CreateDir(const std::string& parentPath,
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

ZMJSON HttpServerModuleFileHub::DeleteItem(const std::string& relativePath,
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

ZMJSON HttpServerModuleFileHub::VerifyDirPassword(const std::string& relativePath,
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

ZMJSON HttpServerModuleFileHub::ChangeDirPassword(const std::string& relativePath,
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

ZMJSON HttpServerModuleFileHub::BatchDelete(const ZMJSON& paths,
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
