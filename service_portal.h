#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H

#include "zm_net_req_loop.h"
#include "zm_net_http_client_pool.h"
#include "module_deepseek.h"
#include "modules/module_user.h"
#include "modules/module_portal.h"

#include <atomic>
#include <string>

class NetDock;
class HttpServerManager;
class FileHubModule;
class ServerAudioStreamModule;   // 业务层自有模块:构造时自建,析构时自删
class DbInitModule;              // 数据库初始化:先于业务模块构造,连接归其所有
class ZmWebSocketSession;        // WebSocket 会话(业务回调形参,完整定义在 cpp)

/**
 * @brief JRPC 请求处理门户，接收 ZmReqLoopPool 分发来的 JRPC 请求并按 method 分发
 *
 * 业务层自有模块(FileHubModule/ServerAudioStreamModule/DeepSeekModule)由本类自建自管,
 * ServiceCenter 不感知;NetDock 只负责网络层(HTTP/JRPC/RESTful)。
 */
class ServicePortal
{
public:
	void SetNetDock(NetDock* nd) { m_netDock = nd; }

	ServicePortal();   // 构造时自建业务模块(文件中心/远程音频/用量统计)
	~ServicePortal();

	/**
	 * @brief 业务层停止钩子(须在 NetDock 析构之前调用)
	 * @note 内部置远程音频 m_tasksGone 与 SSE 测试线程 m_sseGone:NetDock 析构期间
	 *       连接关闭触发 closecb,发送线程可能提前退出,标志未置位时会访问销毁中的
	 *       task/loop(实测崩溃)。ServiceCenter 只感知本钩子,不感知 ServerAudioStreamModule。
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

	// ── WebSocket 业务回调(ServiceCenter 经 NetDock 注入,Open 前设置)──
	/** @brief 连接建立(事件循环线程,轻逻辑) */
	void WebSocketOpenCB(ZmWebSocketSession* session);
	/** @brief 连接关闭(事件循环线程) */
	void WebSocketCloseCB(ZmWebSocketSession* session);
	/** @brief 握手鉴权(事件循环线程;返回 false → 400 不建会话) */
	bool WebSocketAuthCB(const std::string& uri);
	/** @brief 业务消息(ZmReqLoop 线程;回包经 session->PostSendText) */
	void WebSocketMessageCB(ZmWebSocketSession* session, int type,
	                        const BYTE* data, size_t len);

public:
	/** @brief ZmReqLoopPool入口：JRPC 请求回调（在 ZmReqLoop 线程执行） */
	void JrpcRequestReadCB(ZmReqLoop* loop, const char* reqData);

	/** @brief ZmReqLoopPool入口：RESTful 请求回调（在 ZmReqLoop 线程执行） */
	void RestfulRequestCB(ZmReqLoop* loop, const BYTE* body, size_t body_len);

private:
	NetDock* m_netDock = nullptr;
	FileHubModule* m_fileHubModule = nullptr;           // 文件中心:构造时自建,析构时自删
	ServerAudioStreamModule* m_audioModule = nullptr;   // 构造时自建,析构时自删
	// 析构后线程存活窗口(≤2s)内读取本标志属形式 UB,实际为 freed 内存保留 true 值跳投递,测试端点可接受
	std::atomic<bool> m_sseGone {false};   ///< 析构已开始:SSE 测试线程跳过收尾投递(防 loop 已销毁)

	ZmHttpClientPool* m_httpClientPool = nullptr;   ///< 全门户通用外呼请求池(预创建4/上限16)
	DeepSeekModule* m_deepseekModule = nullptr;     ///< DeepSeek 余额查询:构造时自建(注入池),析构时自删

	UserModule* m_userModule = nullptr;             ///< 用户系统:构造时自建(注入已初始化连接),析构时自删
	PortalModule* m_portalModule = nullptr;         ///< 门户模块:构造时自建(注入 UserModule),析构时自删

	DbInitModule* m_dbInit = nullptr;               ///< 数据库初始化:先于一切业务模块构造,最后销毁(连接归其所有)
};

#endif // SERVICE_PORTAL_H
