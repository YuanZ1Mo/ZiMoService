#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"
#include "hub_proxy_manager.h"
#include "zm_net_request_channel.h"

#include <event2/bufferevent.h>

// 前置声明
class BuffereventPairPool;

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 负责 HTTP JSON-RPC 服务器的生命周期，以及内部 JRPC 请求通道的创建与管理。
 * 内部持有 BuffereventPairPool 用于复用 bufferevent_pair，减少高并发下的系统调用和内存分配。
 *
 * 异步处理流程：
 *   ① HTTP JRPC 请求到达（端口 39440）
 *   ② ZmJsonRpcServer → 异步回调 OnJsonRpcAsync（Worker 线程）
 *   ③ 构建请求 JSON → m_channel->Submit() → Worker 立即返回（不阻塞）
 *   ④ 独立等待线程 wait_for future → 事件循环 InjectJrpcRequest → Hub 链处理
 *   ⑤ 响应到达 → reply(result, error) → task->SendDeferredReply() → HTTP 响应
 *
 * Worker 线程不阻塞等待 JRPC 响应，线程池利用率不受内部请求延迟影响。
 */
class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP JSON-RPC 服务器和内部 JRPC 请求通道
     * @param evbase libevent 事件循环基（ZmEvBaseRunLoop 的 event_base）
     * @param hubMgr 已初始化的 Hub 路由层管理器
     * @return true 初始化成功
     */
    bool Open(struct event_base* evbase, HubProxyManager* hubMgr);

    /**
     * @brief 关闭内部通道和 HTTP 服务器
     */
    void Close();

    /** @brief 查询 JRPC 服务器是否正常运行 */
    bool IsOpen() const { return m_httpServerJRPC != nullptr && m_httpServerJRPC->IsRunning(); }

private:
    // ========================================================================
    // 异步 JRPC 回调（ZmJsonRpcServer 的 OnJsonRpcRequestCBAsync）
    // ========================================================================

    /**
     * @brief JSON-RPC 异步回调入口（Worker 线程调用，立即返回）
     *
     * 构建请求并通过内部通道提交。Worker 不阻塞，由独立线程等待响应。
     * 响应到达后调用 reply(result, error) 发送 HTTP 响应。
     */
    void OnJsonRpcAsync(ZmHttpdTask* task, const std::string& method,
                         const ZMJSON& params,
                         std::function<void(const ZMJSON&, const ZMJSON&)> reply);

    // ========================================================================
    // 内部 JRPC 请求通道
    // ========================================================================

    bool OpenJrpcChannel(HubProxyManager* hubMgr);
    void CloseJrpcChannel();

    /** @brief ZmNetRequestChannel handler：bufferevent_pair → Hub 注入 */
    void InjectJrpcRequest(const std::string& request_json,
                            std::function<void(std::string)> callback);

    // ========================================================================
    // bufferevent_pair 响应回调
    // ========================================================================

    static void OnResponseRead(struct bufferevent* bev, void* ctx);
    static void OnResponseEvent(struct bufferevent* bev, short events, void* ctx);

    struct ResponseReadCtx
    {
        std::function<void(std::string)> callback;
        void*                            pool_slot;       ///< 池槽位指针（用于归还 pair[0]）
        uint32_t                         response_len;
        bool                             header_read;
        std::string                      buffer;
    };

    // ========================================================================
    // 成员变量
    // ========================================================================

    struct event_base*   m_evbase;
    ZmJsonRpcServer*     m_httpServerJRPC;
    HubProxyManager*     m_hubMgr;
    ZmNetRequestChannel* m_channel;
    BuffereventPairPool* m_pairPool;         ///< bufferevent_pair 对象池
};

#endif // HTTP_JSONRPC_MANAGER_H
