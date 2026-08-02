#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"
#include "zm_net_req_loop.h"   // ZmReqLoop 基类 + ZmReqLoopPool(池随基类同文件)

// 前向声明（头文件中仅通过指针使用）
class ZmEvBaseRunLoop;

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 负责 ZmJsonRpcServer 和私有 A 池(ZmReqLoopPool)的生命周期管理。
 * 内部持有独立的 ZmEvBaseRunLoop(供 ZmJsonRpcServer 使用)。
 * 请求经 A 池分发给 per-request 事件循环线程(契约 A)执行,
 * 响应经 ZmReqLoopJrpc::Response 直通 task 发送(不绕 pair/Hub)。
 *
 * 异步处理流程(契约 A):
 *   ① HTTP JRPC 请求到达 Worker 线程 → OnJsonRpcCBAsync
 *   ② A 池 Acquire(排队/超时→错误信封,deadline 超时→504)→ task->BindLoop → PostToLoop(START)
 *   ③ A 线程: Bind + JrpcRequestReadCB(loop, reqData)
 *   ④ 业务 ZmReqLoopJrpc::Response 回复(服务器 replyCB 构造信封 + task 直通)
 *   ⑤ TryReply 门 + Release → 回池
 */
class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP/HTTPS JSON-RPC 服务器、A 池
     * @return true 初始化成功
     */
    bool Open();

    /** @brief 关闭 HTTP 服务器、A 池和自有事件循环(软关闭) */
    void Close();

    /** @brief 设置 JRPC 业务回调(NetDock 在 Open 前调用) */
    void SetJrpcRequestReadCB(ZmReqLoopJrpcRequestCB cb) { m_jrpcRequestReadCB = cb; }

    /** @brief 查询 JRPC 服务器是否正常运行 */
    bool IsOpen() const { return m_httpServerJRPC != nullptr && m_httpServerJRPC->IsOpen(); }

    /** @brief 热加载 SSL 证书 */
    bool ReloadCertificate(const char* certFile, const char* keyFile);

    /** @brief 查询是否已启用 HTTPS */
    bool IsHttps() const { return m_httpServerJRPC && m_httpServerJRPC->IsHttps(); }

    /** @brief 设置 TLS session ticket 密钥(转发给内部 ZmJsonRpcServer) */
    void SetTicketKeys(const unsigned char* keys, size_t len);

    /** @brief 投递 ticket 密钥到服务器事件循环线程(轮换场景) */
    void PostSetTicketKeys(const unsigned char* keys, size_t len);

private:
    // ========================================================================
    // 异步 JRPC 回调(ZmJsonRpcServer 的 OnJsonRpcRequestCBAsync)
    // ========================================================================

    /**
     * @brief JSON-RPC 异步回调入口(Worker 线程调用,立即返回)
     *
     * 获取 A 实例并投递 START 到 A 线程,业务回调在其上执行;
     * 响应经 ZmReqLoopJrpc::Response 直通 task 发送 HTTP 响应。
     */
    void OnJsonRpcCBAsync(ZmHttpdTask* task, const ZMJSON& request,
        std::function<void(const ZMJSON& response)> replyCB);

    // ========================================================================
    // 成员变量
    // ========================================================================

    ZmEvBaseRunLoop*          m_evLoopHttpServerJRPC;   ///< 自有事件循环线程(供 ZmJsonRpcServer 使用)
    ZmJsonRpcServer*          m_httpServerJRPC;         ///< HTTP JSON-RPC 服务器实例
    ZmReqLoopPool*            m_reqLoopPool;            ///< 私有 A 池(预创建 + 扩容上限)
    ZmReqLoopJrpcRequestCB    m_jrpcRequestReadCB;      ///< 业务回调(NetDock 注入,Open 前设置)
};

#endif // HTTP_JSONRPC_MANAGER_H
