#include "http_server_manager.h"
#include "http_module_file_hub.h"
#include "service_define.h"

#include "zm_net_http.h"
#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "zm_util_str.h"

#include <algorithm>
#include <fcntl.h>
#include <io.h>
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

bool HttpServerManager::ReloadCertificate(const char* certFile, const char* keyFile)
{
	if (!m_httpServer || !m_httpServer->IsHttps())
		return false;
	return m_httpServer->ReloadCertificate(certFile, keyFile);
}

bool HttpServerManager::Open(const char* wwwRoot,
                              const char* certFile,
                              const char* keyFile)
{
	if (m_httpServer)
		return true;

	if (wwwRoot && wwwRoot[0])
		m_wwwRoot = wwwRoot;

	SetupRouter();

	// 判断是否启用 HTTPS（certFile + keyFile 都非空时启用）
	bool useHttps = (certFile && certFile[0] && keyFile && keyFile[0]);
	uint16_t httpPort = useHttps ? ZM_HTTPS_SERVER_PORT : ZM_HTTP_SERVER_PORT;

	m_evLoop = new ZmEvBaseRunLoop("HttpServerLoop");
	if (!m_evLoop->Loop())
	{
		DEFAULT_LOG_ERROR("HTTP 服务器启动失败：事件循环启动失败");
		delete m_evLoop;
		m_evLoop = nullptr;
		return false;
	}

	// redirect_from_port: HTTPS 模式下从端口 80 做 301 重定向（SSL_CTX 由 ZmHttpServer 内部管理）
	uint16_t redirectPort = useHttps ? ZM_HTTP_SERVER_PORT : 0;
	m_httpServer = new ZmHttpServer(m_evLoop->GetEventBase(), httpPort,
	                                certFile, keyFile, redirectPort,
	                                4096, "HTTP");
	m_httpServer->SetRequestCallback(
		std::bind(&HttpServerManager::OnHttpRequest, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	if (!m_httpServer->Init())
	{
		DEFAULT_LOG_ERROR("{} 服务器初始化失败，端口:{}",
			useHttps ? "HTTPS" : "HTTP", httpPort);
		delete m_httpServer;
		m_httpServer = nullptr;
		m_evLoop->Stop();
		delete m_evLoop;
		m_evLoop = nullptr;
		// sslCtx 由 ZmHttpServer::Close() 释放，此处无需处理
		return false;
	}

	DEFAULT_LOG_INFO("{} 服务器已启动，端口:{}，wwwRoot:{}",
		useHttps ? "HTTPS" : "HTTP", httpPort,
		m_wwwRoot.empty() ? "(无)" : m_wwwRoot);

	// 创建文件中心模块（功能通过 RESTful API 暴露）
	m_fileHub = new HttpModuleFileHub(m_wwwRoot);

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
		return ZM_HTTP_STATUS_CODE_FORBIDDEN;
	std::string normPath = ZmString::Unicode_To_UTF8(normalized);

	WCHAR normRoot[MAX_PATH];
	std::wstring wRoot = ZmString::UTF8_To_Unicode(m_wwwRoot);
	if (!GetFullPathNameW(wRoot.c_str(), MAX_PATH, normRoot, nullptr))
		return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;
	std::string normRootStr = ZmString::Unicode_To_UTF8(normRoot);

	if (normPath.size() < normRootStr.size() ||
	    _strnicmp(normPath.c_str(), normRootStr.c_str(), normRootStr.size()) != 0)
		return ZM_HTTP_STATUS_CODE_FORBIDDEN;

	auto trySendFile = [&](const std::string& path) -> bool {
		int fd = -1;
		if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
			return false;

		int64_t fileSize = _filelengthi64(fd);
		if (fileSize <= 0) { _close(fd); return false; }

		task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(path));

		if (task->SetReplyFile(fd, 0, fileSize) != 0) { _close(fd); return false; }
		return true;
	};

	if (trySendFile(normPath)) return ZM_HTTP_STATUS_CODE_OK;

	// 兜底：文件不存在时转到 404 页面（避免 404 页面自身再次兜底导致无限递归）
	std::string notFoundPath = normRootStr + "\\html\\404.html";
	if (normPath != notFoundPath && trySendFile(notFoundPath)) return ZM_HTTP_STATUS_CODE_NOT_FOUND;

	return ZM_HTTP_STATUS_CODE_NOT_FOUND;
}
