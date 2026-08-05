#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"
#include "zm_net_req_loop.h"   // ZmReqLoopJrpcRequestCB(业务回调类型)

// 前向声明（头文件中仅通过指针使用）
class ZmEvBaseRunLoop;

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 负责 ZmJsonRpcServer 的自有事件循环(ZmEvBaseRunLoop)生命周期管理。
 * ZmReqLoopPool 已下沉到 ZmJsonRpcServer 内部自治
 * (EnableLoopPool/AcquireLoop/DispatchLoop,见 zm_net_http.h),
 * 请求分发(解析 → 信封暂存 → ZmReqLoopPool 分发)全部由服务器类完成,
 * 本管理器仅保留:evLoop 创建/停止、证书与 ticket 转发、业务回调透传。
 *
 * 异步处理流程:
 *   ① HTTP JRPC 请求到达 Worker 线程 → 服务器 OnHttpdRequest 解析
 *   ② 响应信封存入 ZmReqLoopJrpc → 内部 ZmReqLoopPool Acquire(排队/超时→DROPPED 信封,deadline 超时→504)
 *   ③ task->BindLoop → PostToLoop(START) → ZmReqLoop 线程:业务回调(loop, reqData)
 *   ④ 业务 ZmReqLoopJrpc::ResponseJson(loop, rsp) 回复(服务器组装信封 + task 直通)
 *   ⑤ Response 内 TryReply 门 + DONE 投递 → ZmReqLoop 线程 Release → 回池
 */
class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP/HTTPS JSON-RPC 服务器(含内部 ZmReqLoopPool)
     * @return true 初始化成功
     */
    bool Open();

    /** @brief 关闭 HTTP 服务器、ZmReqLoopPool和自有事件循环(软关闭) */
    void Close();

    /** @brief 设置 JRPC 业务回调(NetDock 在 Open 前调用,Open 时透传到服务器) */
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
    ZmEvBaseRunLoop*          m_evLoopHttpServerJRPC;   ///< 自有事件循环线程(供 ZmJsonRpcServer 使用)
    ZmJsonRpcServer*          m_httpServerJRPC;         ///< HTTP JSON-RPC 服务器实例(含内部 ZmReqLoopPool)
    ZmReqLoopJrpcRequestCB    m_jrpcRequestReadCB;      ///< 业务回调(NetDock 注入,Open 前设置)
};

#endif // HTTP_JSONRPC_MANAGER_H
