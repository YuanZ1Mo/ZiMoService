#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"
#include "hub_proxy_manager.h"
#include "zm_net_request_channel.h"

#include <event2/bufferevent.h>

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 负责 HTTP JSON-RPC 服务器的生命周期，以及内部 JRPC 请求通道的创建与管理。
 * 内部请求通过 ZmNetRequestChannel 跨线程交付，handler 中通过 bufferevent_pair
 * 注入 Hub 代理链（协议探测 → JRPC delegate → ServicePortal），响应通过 pair 读回调捕获。
 *
 * 依赖关系：
 *   Worker 线程 → SendToHubProxy → ZmNetRequestChannel::Submit
 *     → 事件循环线程 → InjectJrpcRequest
 *       → bufferevent_pair + OnPairAcceptBev → Hub 代理链
 *         → ServicePortal::JrpcRequestReadCB
 *           → WriteResponse → pair[0] 读回调 → promise.set_value
 */
class HttpJsonRpcManager
{
public:
	HttpJsonRpcManager();
	~HttpJsonRpcManager();

	/**
	 * @brief 初始化 HTTP JSON-RPC 服务器和内部 JRPC 请求通道
	 * @param evbase libevent 事件循环基（DockRunLoop 的 event_base）
	 * @param hubMgr 已初始化的 Hub 路由层管理器
	 * @return true 初始化成功
	 *
	 * 创建 ZmJsonRpcServer（端口 39440）和 ZmNetRequestChannel。
	 * 内部通道的 handler 将请求通过 bufferevent_pair 注入 Hub 代理链。
	 */
	bool Open(struct event_base* evbase, HubProxyManager* hubMgr);

	/**
	 * @brief 关闭内部通道和 HTTP 服务器
	 *
	 * 先关闭 ZmNetRequestChannel（拒绝所有 pending promise，唤醒 Worker 线程），
	 * 再停止 ZmJsonRpcServer（join 线程池）。
	 */
	void Close();

	/** @brief 查询 JRPC 服务器是否正常运行（线程存活） */
	bool IsOpen() const { return m_httpServerJRPC != nullptr && m_httpServerJRPC->IsRunning(); }

private:
	// ========================================================================
	// 内部 JRPC 请求通道
	// ========================================================================

	/**
	 * @brief 打开进程内 JRPC 请求通道（在 Open 中调用）
	 * @param hubMgr Hub 路由层管理器
	 * @return true 成功
	 */
	bool OpenJrpcChannel(HubProxyManager* hubMgr);

	/** @brief 关闭内部 JRPC 请求通道（在 Close 中调用） */
	void CloseJrpcChannel();

	/**
	 * @brief ZmNetRequestChannel 的 handler：将请求通过 bufferevent_pair 注入 Hub 代理链
	 *
	 * 创建 bufferevent_pair → 写入 JRPC 帧到 pair[0] → pair[1] 通过 OnPairAcceptBev
	 * 注入 Hub → Hub 链处理（协议探测 → JRPC delegate → ServicePortal）→
	 * 响应通过 pair[0] 读回调捕获 → 触发 callback。
	 *
	 * @param request_json 请求 JSON 字符串（不含 JRPC 魔术头，由本方法添加）
	 * @param callback     响应回调（空字符串表示错误）
	 */
	void InjectJrpcRequest(const std::string& request_json,
	                        std::function<void(std::string)> callback);

	// ========================================================================
	// HTTP 回调
	// ========================================================================

	/**
	 * @brief 通过内部通道向事件循环线程交付 JRPC 请求并等待响应
	 * @param reqjs  请求 JSON 字符串
	 * @param rspjs  输出：响应 JSON 字符串
	 * @return true 成功，false 失败（通道未就绪、超时或返回空响应）
	 */
	bool SendToHubProxy(const char* reqjs, std::string& rspjs);

	/**
	 * @brief HTTP JSON-RPC 请求回调，由 ZmHttpdDoer Worker 线程调用
	 * @param task   请求上下文对象
	 * @param method JRPC 方法名
	 * @param params JRPC 参数
	 * @param result 输出：JRPC 成功结果
	 * @param error  输出：JRPC 错误信息
	 * @return HTTP 状态码（0 表示成功）
	 */
	int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
	                    const ZMJSON& params, ZMJSON& result, ZMJSON& error);

	// ========================================================================
	// 静态 libevent 回调（bufferevent_pair 响应读取）
	// ========================================================================

	/** @brief pair[0] 读取回调 — 接收 Hub 回写的响应（4字节大端长度 + JSON） */
	static void OnResponseRead(struct bufferevent* bev, void* ctx);
	/** @brief pair[0] 事件回调 — 处理 EOF/ERROR（TAP 异常释放等） */
	static void OnResponseEvent(struct bufferevent* bev, short events, void* ctx);

	/**
	 * @brief 响应读取上下文，跟踪跨多次读回调的拼包状态
	 */
	struct ResponseReadCtx
	{
		std::function<void(std::string)> callback;     ///< 响应就绪后的回调
		struct bufferevent*              pair0;        ///< pair 工作端，读取完毕后释放
		uint32_t                         response_len; ///< 期望的响应长度（从4字节头解析）
		bool                             header_read;  ///< 是否已读取4字节长度头
		std::string                      buffer;       ///< 已读取的响应数据积累
	};

	// ========================================================================
	// 成员变量
	// ========================================================================

	struct event_base*  m_evbase;           ///< libevent 事件循环基（DockRunLoop）
	ZmJsonRpcServer*    m_httpServerJRPC;   ///< HTTP JSON-RPC 服务器
	HubProxyManager*    m_hubMgr;           ///< Hub 路由层（外部注入，不持有所有权）
	ZmNetRequestChannel* m_channel;         ///< 进程内 JRPC 请求通道（本类创建和释放）
};

#endif // HTTP_JSONRPC_MANAGER_H
