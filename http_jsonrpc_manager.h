#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"

// 前向声明（头文件中仅通过指针使用）
class ZmEvBaseRunLoop;
class HubProxyManager;
class BuffereventPairPool;
class ZmNetRequestChannel;
struct bufferevent;

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 负责 HTTP JSON-RPC 服务器的生命周期，以及内部 JRPC 请求通道的创建与管理。
 * 内部持有独立的 ZmEvBaseRunLoop（供 ZmJsonRpcServer 使用），同时持有
 * BuffereventPairPool 用于复用 bufferevent_pair，减少高并发下的系统调用和内存分配。
 *
 * Close() 仅执行软关闭（通道 + HTTP 服务器 + 自有事件循环），pair 池在析构或
 * ShutdownPairPool() 中销毁。调用者须确保在 delete 本对象前已关闭 Hub（所有 TAP
 * 已 Drop），防止在飞请求访问已释放的 pair bufferevent。详见 NetDock::UnInit() 中的关闭顺序。
 *
 * 异步处理流程（零等待线程）：
 *   ① HTTP JRPC 请求到达（端口 39440，自有事件循环线程）
 *   ② ZmJsonRpcServer → 异步回调 OnJsonRpcAsync（Worker 线程）
 *   ③ 构建请求 JSON → m_hub_channel->SubmitAsync() → Worker 立即返回（不阻塞）
 *   ④ Drain() → InjectJrpcRequest（事件循环线程）→ bufferevent_pair 注入 Hub 链
 *   ⑤ Hub 链 → JRPC delegate → 线程池 → 业务逻辑 → Response → pair 回写
 *   ⑥ pair[0] 响应回调 → 直接触发 reply(result, error) → HTTP 响应
 *
 * 全程零额外线程：Worker 不阻塞等待，响应通过 bufferevent_pair 直接在事件循环线程回调。
 */
class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP JSON-RPC 服务器和内部 JRPC 请求通道
     * @param hubMgr 已初始化的 Hub 路由层管理器
     * @return true 初始化成功
     */
    bool Open(HubProxyManager* hubMgr);

    /**
     * @brief 关闭内部通道、HTTP 服务器和自有事件循环（软关闭，不销毁 pair 池）
     *
     * 关闭 ZmNetRequestChannel（拒绝所有 pending promise）并停止 HTTP 服务器和自有事件循环。
     * Pair 池暂不销毁 — 已注入 Hub 链的在飞请求仍需 pair 完成响应回写。
     * Pair 池需在 Hub 事件循环停止前通过 ShutdownPairPool() 显式销毁，
     * 因 bufferevent_free 内部依赖 event_base 存活。
     */
    void Close();

    /**
     * @brief 销毁 bufferevent_pair 对象池（须在 Hub 事件循环停止前调用）
     *
     * 在所有 TAP 已 Drop（pair[1] 已归还）之后、event_base 销毁之前调用。
     * 调用后 m_pairPool 置空，析构函数不再重复销毁。
     */
    void ShutdownPairPool();

    /** @brief 查询 JRPC 服务器是否正常运行 */
    bool IsOpen() const { return m_httpServerJRPC != nullptr && m_httpServerJRPC->IsOpen(); }

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
        bool                             callback_fired;  ///< 防止 OnResponseRead/Event 双回调（pair1 Drop 可能触发 BEV_EVENT_EOF）
        std::string                      buffer;
    };

    // ========================================================================
    // 成员变量
    // ========================================================================

    ZmEvBaseRunLoop*     m_evLoop;           ///< 自有事件循环线程（供 ZmJsonRpcServer 使用）
    ZmJsonRpcServer*     m_httpServerJRPC;   ///< HTTP JSON-RPC 服务器实例
    HubProxyManager*     m_hubMgr;           ///< Hub 路由层（用于 pair pool + channel，非 HTTP 服务器）
    ZmNetRequestChannel* m_hub_channel;      ///< 内部 JRPC 请求通道（走 Hub 事件循环）
    BuffereventPairPool* m_pairPool;         ///< bufferevent_pair 对象池（走 Hub 事件循环）
};

#endif // HTTP_JSONRPC_MANAGER_H
