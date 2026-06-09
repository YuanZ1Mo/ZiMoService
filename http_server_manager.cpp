#include "http_server_manager.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "zm_net_http_middleware.h"

#include <fstream>
#include <ctime>
#include <algorithm>

HttpServerManager::HttpServerManager()
	: m_httpServer(nullptr)
{
}

HttpServerManager::~HttpServerManager()
{
	Close();
}

bool HttpServerManager::Open(const char* wwwRoot)
{
	if (m_httpServer)
		return true;

	if (wwwRoot && wwwRoot[0])
		m_wwwRoot = wwwRoot;

	// 安装中间件 + 静态文件兜底（API 路由由业务层通过 GetRouter() 注册）
	SetupRouter();

	m_httpServer = new ZmHttpServer(80);
	m_httpServer->SetRequestCallback(
		std::bind(&HttpServerManager::OnHttpRequest, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	m_httpServer->Start();

	DEFAULT_LOG_INFO("HTTP 服务器已启动，端口:80，wwwRoot:{}",
		m_wwwRoot.empty() ? "(无)" : m_wwwRoot);
	return true;
}

void HttpServerManager::Close()
{
	if (m_httpServer)
	{
		m_httpServer->Stop();
		delete m_httpServer;
		m_httpServer = nullptr;
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

	// 分块读取发送，固定 64KB 缓冲区，大文件不 OOM
	auto trySendFile = [&](const std::string& path) -> bool {
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
			return false;

		// 获取文件大小
		file.seekg(0, std::ios::end);
		std::streamsize size = file.tellg();
		if (size <= 0) return false;
		file.seekg(0, std::ios::beg);

		task->PutReplyHeader("Content-type", GetMimeType(path));

		char buf[65536];  // 64KB 栈缓冲区
		while (size > 0)
		{
			std::streamsize chunk = (std::min)(size, (std::streamsize)sizeof(buf));
			file.read(buf, chunk);
			task->SetReplyData((const BYTE*)buf, (size_t)chunk);
			size -= chunk;
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
