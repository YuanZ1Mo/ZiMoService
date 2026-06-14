#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"

class NetDock;
class HttpServerManager;

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并按 method 分发
 */
class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};

	void SetNetDock(NetDock* nd) { m_netDock = nd; }

	/**
	 * @brief 注册 HTTP 80 端口路由（供 ServiceCenter 在启动时调用）
	 */
	void RegisterHttpRoutes(HttpServerManager* httpMgr);

public:
	/** @brief TAP 链入口：JRPC 请求回调（在 JRPC delegate 线程池中执行） */
	void JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData);

private:
	NetDock* m_netDock = nullptr;
};

#endif // SERVICE_PORTAL_H
