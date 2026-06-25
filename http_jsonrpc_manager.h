#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"

// 前向声明（头文件中仅通过指针使用）
class ZmEvBaseRunLoop;
class HubProxyManager;
class BuffereventPairPool;
struct BuffereventPairHandle;
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
 * 异步处理流程（零线程模型）：
 *   ① HTTP JRPC 请求到达 Worker 线程
 *   ② OnJsonRpcAsync 直接调用 InjectJrpcRequest（BEV_OPT_THREADSAFE 保证线程安全）
 *   ③ bufferevent_pair[1] 注入 Hub 代理链 → 协议探测 → JRPC delegate
 *   ④ 业务逻辑在线程池中处理 → Response → pair[1] 回写
 *   ⑤ pair[0] 响应回调 → 直接触发 reply(result, error) → HTTP 响应
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
    bool Open();

    /**
     * @brief 关闭 HTTP 服务器和自有事件循环（软关闭，不销毁 pair 池）
     *
     * 停止 HTTP 服务器和自有事件循环。
     * Pair 池暂不销毁 — 已注入 Hub 链的在飞请求仍需 pair 完成响应回写。
     * Pair 池需在 Hub 事件循环停止前通过 ShutdownPairPool() 显式销毁，
     * 因 bufferevent_free 内部依赖 event_base 存活。
     */
    void Close();

    /**
     * @brief 销毁 pair 池（必须在 Hub 清完 TAP 后、event loop 停止前调用）
     *
     * bufferevent_free 内部调用 event_del 需 event_base 存活，
     * 因此必须在 m_evLoopPairPool->Stop() 之前调用。
     * 由 NetDock::CloseHub() 通过 HubProxyManager::Close(beforeLoopStop) 回调触发。
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
     * 构建请求，直接调用 InjectJrpcRequest 写入 bufferevent_pair 并注入 Hub。
     * BEV_OPT_THREADSAFE 保证跨线程操作安全性。
     * 响应到达后调用 reply(result, error) 发送 HTTP 响应。
     */
    void OnJsonRpcCBAsync(ZmHttpdTask* task, const std::string& method,
                         const ZMJSON& params,
                         std::function<void(const ZMJSON&, const ZMJSON&, const ZMJSON&)> reply);

    // ========================================================================
    // bufferevent_pair 响应回调
    // ========================================================================

    static void OnResponseRead(struct bufferevent* bev, void* ctx);
    static void OnResponseEvent(struct bufferevent* bev, short events, void* ctx);

    struct ResponseReadCtx
    {
        std::function<void(std::string)> callback;
        BuffereventPairHandle*           pair_handle;     ///< 池句柄（用于归还 pair[0]）
        uint32_t                         response_len;
        bool                             header_read;
        std::string                      buffer;
    };

    // ========================================================================
    // 成员变量
    // ========================================================================

    ZmEvBaseRunLoop*     m_evLoopHttpServerJRPC;           ///< 自有事件循环线程（供 ZmJsonRpcServer 使用）
    ZmJsonRpcServer*     m_httpServerJRPC;   ///< HTTP JSON-RPC 服务器实例
    ZmEvBaseRunLoop*     m_evLoopPairPool;   ///< libevent 事件循环线程（pairPool 拥有所有权）
    BuffereventPairPool* m_pairPool;         ///< bufferevent_pair 对象池（走 Hub 事件循环）
};

#endif // HTTP_JSONRPC_MANAGER_H
