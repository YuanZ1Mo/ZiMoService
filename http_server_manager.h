#ifndef HTTP_SERVER_MANAGER_H
#define HTTP_SERVER_MANAGER_H

#include "zm_net_http_router.h"

class ZmHttpServer;
class ZmHttpdTask;
class ZmEvBaseRunLoop;
class HttpModuleFileHub;

/**
 * @brief 通用 HTTP 服务器管理器
 *
 * 基于 ZmHttpServer 提供普通 HTTP 服务，内部持有独立的 ZmEvBaseRunLoop。
 * 提供静态文件服务能力。
 * 内部创建并管理 HttpModuleFileHub 的生命周期。
 */
class HttpServerManager
{
public:
	HttpServerManager();
	~HttpServerManager();

	bool Open(const char* wwwRoot = nullptr);
	void Close();

	ZmHttpRouter& GetRouter() { return m_router; }
	bool IsOpen() const;
	const std::string& GetWwwRoot() const { return m_wwwRoot; }

	/** @brief 获取文件中心模块指针（供 ServicePortal 调用 JRPC 方法和上传下载） */
	HttpModuleFileHub* GetFileHub() { return m_fileHub; }

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
	HttpModuleFileHub*   m_fileHub;
};

#endif
