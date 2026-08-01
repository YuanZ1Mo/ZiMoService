#ifndef HTTP_SERVER_MANAGER_H
#define HTTP_SERVER_MANAGER_H

#include "zm_net_http_router.h"

class ZmHttpServer;
class ZmHttpdTask;
class ZmEvBaseRunLoop;

/**
 * @brief 通用 HTTP 服务器管理器（网络层）
 *
 * 基于 ZmHttpServer 提供普通 HTTP 服务，内部持有独立的 ZmEvBaseRunLoop。
 * 提供静态文件服务能力。文件中心等业务模块由 ServicePortal 自建自管。
 */
class HttpServerManager
{
public:
	HttpServerManager();
	~HttpServerManager();

	/**
	 * @brief 启动 HTTP/HTTPS 服务器
	 */
	bool Open();
	void Close();

	ZmHttpRouter& GetRouter() { return m_router; }
	bool IsOpen() const;

	/** @brief 热加载 SSL 证书（无需重启服务），HTTP 模式调用无效果 */
	bool ReloadCertificate(const char* certFile, const char* keyFile);
	const std::string& GetWwwRoot() const { return m_wwwRoot; }

	/**
	 * @brief 从 wwwRoot 目录读取并返回静态文件
	 */
	int ServeStaticFile(ZmHttpdTask* task, const std::string& uri);

private:
	int  OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);
	void SetupRouter();

private:
	ZmEvBaseRunLoop*     m_evLoop;
	ZmHttpServer*        m_httpServer;
	std::string          m_wwwRoot;
	ZmHttpRouter         m_router;
};

#endif
