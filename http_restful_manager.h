#ifndef HTTP_RESTFUL_MANAGER_H
#define HTTP_RESTFUL_MANAGER_H

#include "zm_net_http.h"
#include "zm_net_tap_rest.h"

// 前向声明
class ZmEvBaseRunLoop;
class ZmBuffereventPairPool;
struct ZmBuffereventPairHandle;
struct bufferevent;

/**
 * @brief HTTP RESTful 前端管理器
 *
 * 负责 ZmRESTfulServer 和 bufferevent_pair 池的生命周期管理。
 * 内部持有独立的 ZmEvBaseRunLoop（供 ZmRESTfulServer 使用）。
 * 请求到达时打包 RESTful 帧并通过 pair 注入 Hub 代理链。
 *
 * 响应路径（与 JRPC 的关键区别）:
 *   JRPC: 业务层 → pair 回写 → pair[0] 回调 → reply() → HTTP 响应
 *   RESTful: 业务层 → tap->httpd_task->TriggerReply() → HTTP 响应（直通，不绕 pair）
 *
 * 异步处理流程:
 *   ① HTTP RESTful 请求到达 Worker 线程
 *   ② OnRESTfulCBAsync 构建帧 → 写 pair[0] → SetPendingTask(bev1, task)
 *   ③ pair[1] 注入 Hub → 协议探测 → ZmTapDelegateRESTful
 *   ④ delegate 解帧、取出 httpd_task 绑定到 tap
 *   ⑤ 业务回调(tap, meta, body, body_len) 在工作线程执行
 *   ⑥ 业务层直接 tap->httpd_task->TriggerReply() 发送 HTTP 响应
 */
class HttpRestfulManager
{
public:
    HttpRestfulManager();
    ~HttpRestfulManager();

    /**
     * @brief 初始化 HTTP/HTTPS RESTful 服务器、pair 池、delegate
     * @return true 初始化成功
     */
    bool Open();

    /** @brief 关闭 HTTP 服务器和自有事件循环（软关闭） */
    void Close();

    /** @brief 销毁 pair 池（必须在 Hub 清完 TAP 后调用） */
    void ShutdownPairPool();

    /** @brief 查询服务器是否正常运行 */
    bool IsOpen() const { return m_httpServerRESTful != nullptr && m_httpServerRESTful->IsOpen(); }

    /** @brief 热加载 SSL 证书 */
    bool ReloadCertificate(const char* certFile, const char* keyFile);

private:
    // ========================================================================
    // pair[0] 清理回调（仿 JRPC 的 OnResponseEvent，但 RESTful 不通过 pair 回传响应）
    // ========================================================================

    struct PairCleanupCtx
    {
        ZmBuffereventPairHandle* handle;
    };

    static void OnPair0Event(struct bufferevent* bev, short events, void* ctx);

    // ========================================================================
    // 异步回调
    // ========================================================================
    /** @brief RESTful 异步回调（由 ZmRESTfulServer 调用，打包帧并注入 Hub） */
    void OnRESTfulCBAsync(ZmHttpdTask* task, const BYTE* body, size_t body_len);

    // ========================================================================
    // 成员变量
    // ========================================================================

    ZmEvBaseRunLoop*       m_evLoopHttpServer;     ///< 自有事件循环线程（供 ZmRESTfulServer 使用）
    ZmRESTfulServer*       m_httpServerRESTful;     ///< HTTP RESTful 服务器实例
    ZmEvBaseRunLoop*       m_evLoopPairPool;       ///< 事件循环（pairPool 使用）
    ZmBuffereventPairPool* m_pairPool;             ///< bufferevent_pair 对象池
};

#endif // HTTP_RESTFUL_MANAGER_H
