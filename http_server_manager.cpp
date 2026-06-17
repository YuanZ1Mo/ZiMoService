#include "http_server_manager.h"
#include "http_server_module_file_hub.h"

#include "zm_net_http.h"
#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "zm_util_str.h"

#include <event2/buffer.h>

#include <algorithm>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include <share.h>

HttpServerManager::HttpServerManager()
	: m_evLoop(nullptr)
	, m_httpServer(nullptr)
	, m_fileHub(nullptr)
{
}

HttpServerManager::~HttpServerManager()
{
	Close();
}

bool HttpServerManager::IsOpen() const
{
	return m_httpServer != nullptr && m_httpServer->IsOpen();
}

bool HttpServerManager::Open(const char* wwwRoot)
{
	if (m_httpServer)
		return true;

	if (wwwRoot && wwwRoot[0])
		m_wwwRoot = wwwRoot;

	SetupRouter();

	m_evLoop = new ZmEvBaseRunLoop("HttpServerLoop");
	if (!m_evLoop->Loop())
	{
		DEFAULT_LOG_ERROR("HTTP 服务器启动失败：事件循环启动失败");
		delete m_evLoop;
		m_evLoop = nullptr;
		return false;
	}

	m_httpServer = new ZmHttpServer(m_evLoop->GetEventBase(), 80);
	m_httpServer->SetRequestCallback(
		std::bind(&HttpServerManager::OnHttpRequest, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	if (!m_httpServer->Init())
	{
		DEFAULT_LOG_ERROR("HTTP 服务器初始化失败，端口:80");
		delete m_httpServer;
		m_httpServer = nullptr;
		m_evLoop->Stop();
		delete m_evLoop;
		m_evLoop = nullptr;
		return false;
	}

	DEFAULT_LOG_INFO("HTTP 服务器已启动，端口:80，wwwRoot:{}",
		m_wwwRoot.empty() ? "(无)" : m_wwwRoot);

	// 创建文件中心模块并注册路由
	m_fileHub = new HttpServerModuleFileHub(m_wwwRoot);
	m_fileHub->RegisterHttpRoutes(m_router, this);

	return true;
}

void HttpServerManager::Close()
{
	if (m_httpServer)
	{
		m_httpServer->Close();
		delete m_httpServer;
		m_httpServer = nullptr;
	}

	if (m_evLoop)
	{
		m_evLoop->Stop();
		delete m_evLoop;
		m_evLoop = nullptr;
	}

	if (m_fileHub)
	{
		delete m_fileHub;
		m_fileHub = nullptr;
	}

	DEFAULT_LOG_INFO("HTTP 服务器已关闭");
}

// ============================================================================
// 路由注册
// ============================================================================

void HttpServerManager::SetupRouter()
{
	m_router.Use(ZmHttpMiddlewareLogging());
	m_router.Use(ZmHttpMiddlewareRecovery());

	if (!m_wwwRoot.empty())
	{
		m_router.Any("/html/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		m_router.Any("/css/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		m_router.Any("/js/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		// 文件中心路由由 HttpServerModuleFileHub 外部注册
	}
}

// ============================================================================
// 请求分发
// ============================================================================

int HttpServerManager::OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
{
	return m_router.Serve(task, data, dlen);
}

// ============================================================================
// 静态文件服务
// ============================================================================

int HttpServerManager::ServeStaticFile(ZmHttpdTask* task, const std::string& uri)
{
	std::string filePath = (uri == "/" || uri.empty()) ? "/html/index.html" : uri;
	if (!filePath.empty() && filePath[0] == '/')
		filePath = filePath.substr(1);

	std::string rawPath = m_wwwRoot + "\\" + filePath;
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	std::wstring wRaw = ZmString::UTF8_To_Unicode(rawPath);
	WCHAR normalized[MAX_PATH];
	if (!GetFullPathNameW(wRaw.c_str(), MAX_PATH, normalized, nullptr))
		return 403;
	std::string normPath = ZmString::Unicode_To_UTF8(normalized);

	WCHAR normRoot[MAX_PATH];
	std::wstring wRoot = ZmString::UTF8_To_Unicode(m_wwwRoot);
	if (!GetFullPathNameW(wRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return 500;
	std::string normRootStr = ZmString::Unicode_To_UTF8(normRoot);

	if (normPath.size() < normRootStr.size() ||
	    _strnicmp(normPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return 403;

	auto trySendFile = [&](const std::string& path) -> bool {
		int fd = -1;
		if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return false;

		int64_t fileSize = _filelengthi64(fd);
		if (fileSize <= 0) { _close(fd); return false; }

		task->PutReplyHeader("Content-type", GetMimeType(path));

		if (task->SetReplyFile(fd, 0, fileSize) != 0) { _close(fd); return false; }
		return true;
	};

	if (trySendFile(normPath)) return 200;

	// 兜底：文件不存在时转到 404 页面（避免 404 页面自身再次兜底导致无限递归）
	std::string notFoundPath = normRootStr + "\\html\\404.html";
	if (normPath != notFoundPath && trySendFile(notFoundPath)) return 404;

	return 404;
}

// ============================================================================
// 通用文件下载 / 上传
// ============================================================================

std::string HttpServerManager::ExtractFilename(const std::string& uri)
{
	std::string path = uri;
	size_t qpos = path.find('?');
	if (qpos != std::string::npos) path = path.substr(0, qpos);
	size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos) return path.substr(slash + 1);
	return path;
}

int HttpServerManager::ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
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
		task->SetReply(416, "Range Not Satisfiable");
		task->PutReplyHeader("Content-Range", ("bytes */" + std::to_string(fileSize)).c_str());
		return 416;
	}

	int64_t rangeLength = end - start + 1;
	int fd = -1;
	if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return 404;

	task->PutReplyHeader("Content-type", GetMimeType(path));
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + ExtractFilename(path) + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");
	task->PutReplyHeader("Content-Range",
		("bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize)).c_str());
	task->SetReply(206, "Partial Content");

	if (task->SetReplyFile(fd, start, rangeLength) != 0) { _close(fd); return 500; }
	return 206;
}

int HttpServerManager::SendFile(ZmHttpdTask* task, const std::string& physicalPath)
{
	int fd = -1;
	if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return 404;

	int64_t fileSize = _filelengthi64(fd);
	if (fileSize <= 0) { _close(fd); return 404; }

	const char* rangeHeader = task->GetRequestHeader("Range");
	if (rangeHeader && rangeHeader[0]) {
		_close(fd);
		int rangeResult = ServeFileWithRange(task, physicalPath, rangeHeader, fileSize);
		if (rangeResult > 0) return rangeResult;
		if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return 404;
	}

	task->PutReplyHeader("Content-type", GetMimeType(physicalPath));
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + ExtractFilename(physicalPath) + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");

	if (task->SetReplyFile(fd, 0, fileSize) != 0) { _close(fd); return 500; }
	return 200;
}

int HttpServerManager::ReceiveFile(ZmHttpdTask* task, const std::string& physicalPath,
	const BYTE* data, size_t dlen)
{
	if (!data || dlen == 0) return 400;

	// 确保父目录存在
	std::string dirPath = physicalPath;
	size_t lastSlash = dirPath.find_last_of("\\/");
	if (lastSlash != std::string::npos) {
		dirPath = dirPath.substr(0, lastSlash);
		std::wstring wDir = ZmString::UTF8_To_Unicode(dirPath);
		if (!CreateDirectoryW(wDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
			DEFAULT_LOG_ERROR("创建上传目录失败: {}", dirPath);
			return 500;
		}
	}

	std::wstring wPath = ZmString::UTF8_To_Unicode(physicalPath);
	HANDLE hFile = CreateFileW(wPath.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		DEFAULT_LOG_ERROR("创建上传文件失败: {}", physicalPath);
		return 500;
	}

	LARGE_INTEGER liSize;
	liSize.QuadPart = (LONGLONG)dlen;
	if (!SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN) || !SetEndOfFile(hFile)) {
		CloseHandle(hFile); return 500;
	}

	HANDLE hMapping = CreateFileMappingW(hFile, NULL, PAGE_READWRITE,
		liSize.HighPart, liSize.LowPart, NULL);
	if (!hMapping) { CloseHandle(hFile); return 500; }

	BYTE* mappedView = (BYTE*)MapViewOfFile(hMapping, FILE_MAP_WRITE, 0, 0, dlen);
	if (!mappedView) { CloseHandle(hMapping); CloseHandle(hFile); return 500; }

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
	return 201;
}

// ============================================================================
// 工具函数
// ============================================================================

const char* HttpServerManager::GetMimeType(const std::string& path)
{
	size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return "application/octet-stream";

	std::string ext = path.substr(dot);
	for (auto& c : ext) c = (char)tolower((unsigned char)c);

	if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
	if (ext == ".css")                    return "text/css; charset=utf-8";
	if (ext == ".js")                     return "application/javascript; charset=utf-8";
	if (ext == ".json")                   return "application/json; charset=utf-8";
	if (ext == ".png")                    return "image/png";
	if (ext == ".jpg" || ext == ".jpeg")  return "image/jpeg";
	if (ext == ".gif")                    return "image/gif";
	if (ext == ".svg")                    return "image/svg+xml";
	if (ext == ".ico")                    return "image/x-icon";
	if (ext == ".woff")                   return "font/woff";
	if (ext == ".woff2")                  return "font/woff2";
	if (ext == ".ttf")                    return "font/ttf";
	if (ext == ".txt")                    return "text/plain; charset=utf-8";
	if (ext == ".xml")                    return "application/xml; charset=utf-8";
	if (ext == ".pdf")                    return "application/pdf";
	if (ext == ".zip")                    return "application/zip";

	return "application/octet-stream";
}
