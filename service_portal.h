#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"
#include <string>

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
	 * @brief 向所有匹配 tag 的客户端广播消息
	 * @param topic   主题
	 * @param content 内容（JSON 字符串）
	 * @param tag     过滤标签，空字符串表示全部推送
	 * @return true 成功投递，false 广播服务未运行或参数无效
	 */
	bool BroadcastMessage(const std::string& topic, const std::string& content, const std::string& tag);

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
