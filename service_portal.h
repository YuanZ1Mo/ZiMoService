#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H

#include "zm_net_tap.h"

#include <string>

class NetDock;
class HttpServerManager;
class FileHubModule;
class ServerAudioStreamModule;   // 业务层自有模块:构造时自建,析构时自删

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并按 method 分发
 *
 * 业务层自有模块(FileHubModule/ServerAudioStreamModule)由本类自建自管,
 * ServiceCenter 不感知;NetDock 只负责网络层(HTTP/JRPC/RESTful/Hub)。
 */
class ServicePortal
{
public:
	void SetNetDock(NetDock* nd) { m_netDock = nd; }

	ServicePortal();   // 构造时自建业务模块(文件中心/远程音频)
	~ServicePortal();

	/**
	 * @brief 业务层停止钩子(须在 NetDock 析构之前调用)
	 * @note 内部置远程音频 m_tasksGone:NetDock 析构期间连接关闭触发 closecb,
	 *       发送线程可能提前退出,标志未置位时会访问销毁中的 task/tap(实测崩溃)。
	 *       ServiceCenter 只感知本钩子,不感知 ServerAudioStreamModule。
	 */
	void Shutdown();

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

	/** @brief TAP 链入口：REFTful 请求回调（在 REFTful delegate 线程池中执行） */
	void RestfulRequestCB(ZM_TAP_CTX* tap, const BYTE* body, size_t body_len);

private:
	NetDock* m_netDock = nullptr;
	FileHubModule* m_fileHubModule = nullptr;           // 文件中心:构造时自建,析构时自删
	ServerAudioStreamModule* m_audioModule = nullptr;   // 构造时自建,析构时自删
};

#endif // SERVICE_PORTAL_H
