#include "http_server_manager.h"

#include "zm_net_http.h"
#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"

#include <event2/buffer.h>

#include <algorithm>
#include <io.h>
#include <fcntl.h>
#include <share.h>

HttpServerManager::HttpServerManager()
	: m_evLoop(nullptr)
	, m_httpServer(nullptr)
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

	// 安装中间件 + 静态文件兜底（API 路由由业务层通过 GetRouter() 注册）
	SetupRouter();

	// 创建并启动独立的事件循环线程
	m_evLoop = new ZmEvBaseRunLoop("HttpServerLoop");
	if (!m_evLoop->Loop())
	{
		DEFAULT_LOG_ERROR("HTTP 服务器启动失败：事件循环启动失败");
		delete m_evLoop;
		m_evLoop = nullptr;
		return false;
	}

	// 将 HTTP 服务器绑定到自己的事件循环
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
	return true;
}

void HttpServerManager::Close()
{
	// ★ 先停 HTTP 服务器（join 线程池，释放 evhttp）
	if (m_httpServer)
	{
		m_httpServer->Close();
		delete m_httpServer;
		m_httpServer = nullptr;
	}

	// 再停事件循环线程（evhttp 已释放，evbase 安全释放）
	if (m_evLoop)
	{
		m_evLoop->Stop();
		delete m_evLoop;
		m_evLoop = nullptr;
		DEFAULT_LOG_INFO("HTTP 服务器已关闭");
	}
}

// ============================================================================
// 路由注册
// ============================================================================

void HttpServerManager::SetupRouter()
{
	// --- 全局中间件 ---
	m_router.Use(ZmHttpMiddlewareLogging());
	m_router.Use(ZmHttpMiddlewareRecovery());

	// --- 按目录放行静态文件（精确路由由业务层通过 GetRouter() 在外部注册）---
	if (!m_wwwRoot.empty())
	{
		// html 目录：页面文件
		m_router.Any("/html/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		// css 目录：样式表
		m_router.Any("/css/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		// js 目录：脚本
		m_router.Any("/js/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeStaticFile(task, uri);
		});
		// download 目录：文件下载（Content-Disposition: attachment）
		m_router.Any("/download/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeDownloadFile(task, uri);
		});
		// upload 目录：文件上传（POST 请求体写入磁盘）
		m_router.Post("/upload/*", [this](ZmHttpdTask* task, const BYTE* data, size_t dlen) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			return ServeUpload(task, uri, data, dlen);
		});
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
	// 确定文件路径：/ → /html/index.html
	std::string filePath = (uri == "/" || uri.empty()) ? "/html/index.html" : uri;
	if (!filePath.empty() && filePath[0] == '/')
		filePath = filePath.substr(1);

	std::string rawPath = m_wwwRoot + "\\" + filePath;
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	// 安全校验：GetFullPathNameA 规范化路径（解析 .. 和 .），前缀比对防穿越
	char normalized[MAX_PATH];
	if (!GetFullPathNameA(rawPath.c_str(), MAX_PATH, normalized, nullptr))
		return 403;
	std::string normPath(normalized);

	char normRoot[MAX_PATH];
	if (!GetFullPathNameA(m_wwwRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return 500;
	std::string normRootStr(normRoot);

	if (normPath.size() < normRootStr.size() ||
	    _strnicmp(normPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return 403;

	// 零拷贝发送文件：通过 evbuffer_file_segment（mmap/MapViewOfFile）避免用户态拷贝
	auto trySendFile = [&](const std::string& path) -> bool {
		int fd = -1;
		if (_sopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return false;

		int64_t fileSize = _filelengthi64(fd);
		if (fileSize <= 0)
		{
			_close(fd);
			return false;
		}

		task->PutReplyHeader("Content-type", GetMimeType(path));

		if (task->SetReplyFile(fd, 0, fileSize) != 0)
		{
			_close(fd);
			return false;
		}

		return true;
	};

	if (trySendFile(normPath))
		return 200;

	// SPA 兜底
	std::string indexPath = normRootStr + "\\html\\index.html";
	if (trySendFile(indexPath))
		return 200;

	return 404;
}

// ============================================================================
// 文件下载服务
// ============================================================================

std::string HttpServerManager::ExtractFilename(const std::string& uri)
{
	// 去掉 query string（如果有）
	std::string path = uri;
	size_t qpos = path.find('?');
	if (qpos != std::string::npos)
		path = path.substr(0, qpos);

	// 取最后一个 '/' 或 '\' 之后的部分
	size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos)
		return path.substr(slash + 1);
	return path;
}

int HttpServerManager::ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
	const std::string& rangeStr, int64_t fileSize)
{
	// 解析 Range: bytes=start-end
	// 支持三种格式：
	//   "bytes=0-499"  → 前 500 字节
	//   "bytes=500-"   → 从 500 到末尾
	//   "bytes=-500"   → 最后 500 字节
	if (rangeStr.size() < 7 || _strnicmp(rangeStr.c_str(), "bytes=", 6) != 0)
		return -1;  // 无法解析，回退完整文件

	std::string rangeVal = rangeStr.substr(6);  // 去掉 "bytes="

	// 仅支持单 Range，多 Range（含逗号）回退完整文件
	if (rangeVal.find(',') != std::string::npos)
		return -1;

	size_t dashPos = rangeVal.find('-');
	if (dashPos == std::string::npos)
		return -1;

	int64_t start = 0;
	int64_t end = fileSize - 1;

	std::string startStr = rangeVal.substr(0, dashPos);
	std::string endStr = rangeVal.substr(dashPos + 1);

	if (startStr.empty() && !endStr.empty())
	{
		// "bytes=-500" → 最后 suffixLen 字节
		int64_t suffixLen = std::stoll(endStr);
		if (suffixLen >= fileSize)
			start = 0;
		else
			start = fileSize - suffixLen;
		end = fileSize - 1;
	}
	else if (!startStr.empty() && endStr.empty())
	{
		// "bytes=500-" → 从 start 到末尾
		start = std::stoll(startStr);
		end = fileSize - 1;
	}
	else
	{
		// "bytes=0-499" → 指定区间
		start = std::stoll(startStr);
		end = std::stoll(endStr);
	}

	// 校验范围合法性
	if (start < 0 || end >= fileSize || start > end)
	{
		// 416 Range Not Satisfiable
		task->SetReply(416, "Range Not Satisfiable");
		task->PutReplyHeader("Content-Range",
			("bytes */" + std::to_string(fileSize)).c_str());
		return 416;
	}

	int64_t rangeLength = end - start + 1;

	// 使用 CRT _sopen_s 打开文件，fd 传递给 SetReplyFile 实现零拷贝传输
	int fd = -1;
	if (_sopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return 404;

	// 设置 206 Partial Content 响应头
	task->PutReplyHeader("Content-type", GetMimeType(path));
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + ExtractFilename(path) + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");
	task->PutReplyHeader("Content-Range",
		("bytes " + std::to_string(start) + "-" +
		std::to_string(end) + "/" + std::to_string(fileSize)).c_str());
	task->SetReply(206, "Partial Content");

	// 零拷贝：evbuffer_file_segment 记录 fd 引用，由 libevent 在发送时通过 mmap 读取
	if (task->SetReplyFile(fd, start, rangeLength) != 0)
	{
		_close(fd);
		return 500;
	}

	return 206;
}

int HttpServerManager::ServeDownloadFile(ZmHttpdTask* task, const std::string& uri)
{
	// 确定文件路径：/download/xxx → download/xxx
	std::string filePath = uri;
	if (!filePath.empty() && filePath[0] == '/')
		filePath = filePath.substr(1);

	std::string rawPath = m_wwwRoot + "\\" + filePath;
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	// 安全校验：GetFullPathNameA 规范化路径（解析 .. 和 .），前缀比对防穿越
	char normalized[MAX_PATH];
	if (!GetFullPathNameA(rawPath.c_str(), MAX_PATH, normalized, nullptr))
		return 403;
	std::string normPath(normalized);

	char normRoot[MAX_PATH];
	if (!GetFullPathNameA(m_wwwRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return 500;
	std::string normRootStr(normRoot);

	if (normPath.size() < normRootStr.size() ||
	    _strnicmp(normPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return 403;

	// 使用 CRT _sopen_s 打开文件，fd 传递给 SetReplyFile 实现零拷贝传输
	int fd = -1;
	if (_sopen_s(&fd, normPath.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
		return 404;

	// 获取文件大小
	int64_t fileSize = _filelengthi64(fd);
	if (fileSize <= 0)
	{
		_close(fd);
		return 404;
	}

	// 检查 HTTP Range 请求头（断点续传）
	const char* rangeHeader = task->GetRequestHeader("Range");
	if (rangeHeader && rangeHeader[0])
	{
		_close(fd);
		int rangeResult = ServeFileWithRange(task, normPath, rangeHeader, fileSize);
		if (rangeResult > 0)
			return rangeResult;
		// rangeResult < 0 表示无法解析，回退完整文件下载：重新打开文件
		if (_sopen_s(&fd, normPath.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return 404;
	}

	// 设置下载响应头：MIME 类型 + 强制下载 + 支持断点续传
	task->PutReplyHeader("Content-type", GetMimeType(normPath));
	std::string filename = ExtractFilename(uri);
	task->PutReplyHeader("Content-Disposition",
		("attachment; filename=\"" + filename + "\"").c_str());
	task->PutReplyHeader("Accept-Ranges", "bytes");

	// 零拷贝：evbuffer_file_segment 记录 fd 引用，由 libevent 在发送时通过 mmap 读取
	if (task->SetReplyFile(fd, 0, fileSize) != 0)
	{
		_close(fd);
		return 500;
	}

	return 200;
}

// ============================================================================
// 文件上传服务
// ============================================================================

int HttpServerManager::ServeUpload(ZmHttpdTask* task, const std::string& uri,
	const BYTE* data, size_t dlen)
{
	// 请求体为空则返回 400
	if (!data || dlen == 0)
		return 400;

	// 确定文件路径：/upload/xxx → upload/xxx
	std::string filePath = uri;
	if (!filePath.empty() && filePath[0] == '/')
		filePath = filePath.substr(1);

	std::string rawPath = m_wwwRoot + "\\" + filePath;
	std::replace(rawPath.begin(), rawPath.end(), '/', '\\');

	// 安全校验：GetFullPathNameA 规范化路径（解析 .. 和 .），前缀比对防穿越
	char normalized[MAX_PATH];
	if (!GetFullPathNameA(rawPath.c_str(), MAX_PATH, normalized, nullptr))
		return 403;
	std::string normPath(normalized);

	char normRoot[MAX_PATH];
	if (!GetFullPathNameA(m_wwwRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return 500;
	std::string normRootStr(normRoot);

	if (normPath.size() < normRootStr.size() ||
		_strnicmp(normPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return 403;

	// 确保目标目录存在
	std::string dirPath = normPath;
	size_t lastSlash = dirPath.find_last_of("\\/");
	if (lastSlash != std::string::npos)
	{
		dirPath = dirPath.substr(0, lastSlash);
		if (!CreateDirectoryA(dirPath.c_str(), nullptr) &&
			GetLastError() != ERROR_ALREADY_EXISTS)
		{
			DEFAULT_LOG_ERROR("创建上传目录失败: {}", dirPath);
			return 500;
		}
	}

	// 零拷贝写入：CreateFileMapping + MapViewOfFile 将磁盘页缓存直接映射到用户态
	// evbuffer_copyout 一步从网络缓冲区复制到页缓存，免除中间缓冲区和_write()系统调用
	HANDLE hFile = CreateFileA(normPath.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DEFAULT_LOG_ERROR("创建上传文件失败: {}", normPath);
		return 500;
	}

	// 预分配文件大小，否则 CreateFileMapping 对空文件会失败
	LARGE_INTEGER liSize;
	liSize.QuadPart = (LONGLONG)dlen;
	if (!SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN) || !SetEndOfFile(hFile))
	{
		CloseHandle(hFile);
		return 500;
	}

	HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READWRITE,
		liSize.HighPart, liSize.LowPart, NULL);
	if (!hMapping)
	{
		CloseHandle(hFile);
		return 500;
	}

	BYTE* mappedView = (BYTE*)MapViewOfFile(hMapping, FILE_MAP_WRITE, 0, 0, dlen);
	if (!mappedView)
	{
		CloseHandle(hMapping);
		CloseHandle(hFile);
		return 500;
	}

	// 从网络缓冲区一步复制到页缓存映射内存，小文件用 data 指针，大文件用 evbuffer
	struct evbuffer* inbuf = task->GetInputBuffer();
	if (inbuf && evbuffer_get_length(inbuf) >= dlen)
	{
		evbuffer_copyout(inbuf, mappedView, dlen);
		evbuffer_drain(inbuf, dlen);
	}
	else
	{
		memcpy(mappedView, data, dlen);
	}

	UnmapViewOfFile(mappedView);
	CloseHandle(hMapping);
	CloseHandle(hFile);

	DEFAULT_LOG_INFO("文件上传成功: {} ({} bytes)", normPath, dlen);
	return 201;
}

const char* HttpServerManager::GetMimeType(const std::string& path)
{
	// 提取扩展名
	size_t dot = path.find_last_of('.');
	if (dot == std::string::npos)
		return "application/octet-stream";

	std::string ext = path.substr(dot);
	// 转小写比较
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
