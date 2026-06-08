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

	// --- 静态文件兜底（API 路由由业务层通过 GetRouter() 在外部注册）---
	if (!m_wwwRoot.empty())
	{
		m_router.Any("/*", [this](ZmHttpdTask* task, const BYTE*, size_t) {
			std::string uri(task->Uri() ? task->Uri() : "/");
			size_t q = uri.find('?');
			if (q != std::string::npos) uri = uri.substr(0, q);
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
	// 安全校验：阻止路径穿越
	if (uri.find("..") != std::string::npos)
		return 403;

	// 确定文件路径：/ → /index.html，否则直接拼接
	std::string filePath = (uri == "/" || uri.empty()) ? "/index.html" : uri;

	// 去掉开头的 /
	if (!filePath.empty() && filePath[0] == '/')
		filePath = filePath.substr(1);

	std::string fullPath = m_wwwRoot + "\\" + filePath;
	// 统一使用 / 分隔符（兼容 Windows）
	std::replace(fullPath.begin(), fullPath.end(), '/', '\\');

	// 尝试打开文件
	std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		// 非 API 路径且文件不存在 → SPA 兜底，返回 index.html
		std::string indexPath = m_wwwRoot + "\\index.html";
		std::ifstream indexFile(indexPath, std::ios::binary | std::ios::ate);
		if (!indexFile.is_open())
			return 404;

		std::streamsize size = indexFile.tellg();
		indexFile.seekg(0, std::ios::beg);

		ZmByteBuffer buf((size_t)size);
		indexFile.read((char*)buf.Head(), size);
		indexFile.close();

		task->PutReplyHeader("Content-type", "text/html; charset=utf-8");
		task->SetReplyData(buf.Head(), buf.Size());
		return 200;
	}

	// 读取文件内容
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);

	ZmByteBuffer buf((size_t)size);
	file.read((char*)buf.Head(), size);
	file.close();

	// 设置 MIME 类型
	task->PutReplyHeader("Content-type", GetMimeType(filePath));
	task->SetReplyData(buf.Head(), buf.Size());
	return 200;
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
