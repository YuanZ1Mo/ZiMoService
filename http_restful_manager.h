#ifndef HTTP_RESTFUL_MANAGER_H
#define HTTP_RESTFUL_MANAGER_H

#include "zm_net_http.h"
#include "zm_net_req_loop.h"   // ZmReqLoop 基类 + ZmReqLoopPool(池随基类同文件)

// 前向声明
class ZmEvBaseRunLoop;

/**
 * @brief HTTP RESTful 前端管理器
 *
 * 负责 ZmRESTfulServer 和私有 A 池(ZmReqLoopPool)的生命周期管理。
 * 内部持有独立的 ZmEvBaseRunLoop(供 ZmRESTfulServer 使用)。
 * 请求经 A 池分发给 per-request 事件循环线程(契约 A)执行,
 * 响应经 ZmReqLoopRest::Response* 直通 task 发送(不绕 pair/Hub)。
 *
 * 异步处理流程(契约 A):
 *   ① HTTP RESTful 请求到达 Worker 线程 → OnRESTfulCBAsync
 *   ② A 池 Acquire(排队/超时→503)→ task->BindLoop → PostToLoop(START)
 *   ③ A 线程: Bind + RestfulRequestCB(loop, body, len)
 *   ④ 业务在 A 线程分段推进 → ZmReqLoopRest::Response* 直通 task 回复
 *   ⑤ 回复 helper 内部:TryReply 门 + Release → 回池
 */
class HttpRestfulManager
{
public:
    HttpRestfulManager();
    ~HttpRestfulManager();

    /**
     * @brief 初始化 HTTP/HTTPS RESTful 服务器、A 池
     * @return true 初始化成功
     */
    bool Open();

    /** @brief 关闭 HTTP 服务器和自有事件循环(软关闭) */
    void Close();

    /** @brief 设置 RESTful 业务回调(NetDock 在 Open 前调用) */
    void SetRESTfulRequestCB(ZmReqLoopRestfulRequestCB cb) { m_restfulRequestCB = cb; }

    /** @brief 查询服务器是否正常运行 */
    bool IsOpen() const { return m_httpServerRESTful != nullptr && m_httpServerRESTful->IsOpen(); }

    /** @brief 热加载 SSL 证书 */
    bool ReloadCertificate(const char* certFile, const char* keyFile);

    /** @brief 查询是否已启用 HTTPS */
    bool IsHttps() const { return m_httpServerRESTful && m_httpServerRESTful->IsHttps(); }

    /** @brief 设置 TLS session ticket 密钥(转发给内部 ZmRESTfulServer) */
    void SetTicketKeys(const unsigned char* keys, size_t len);

    /** @brief 投递 ticket 密钥到服务器事件循环线程(轮换场景) */
    void PostSetTicketKeys(const unsigned char* keys, size_t len);

private:
    // ========================================================================
    // 异步回调
    // ========================================================================
    /** @brief RESTful 异步回调(由 ZmRESTfulServer 调用,投递 A 池处理) */
    void OnRESTfulCBAsync(ZmHttpdTask* task, const BYTE* body, size_t body_len);

    // ========================================================================
    // 成员变量
    // ========================================================================

    ZmEvBaseRunLoop*          m_evLoopHttpServer;   ///< 自有事件循环线程(供 ZmRESTfulServer 使用)
    ZmRESTfulServer*          m_httpServerRESTful;   ///< HTTP RESTful 服务器实例
    ZmReqLoopPool*            m_reqLoopPool;         ///< 私有 A 池(预创建 + 扩容上限)
    ZmReqLoopRestfulRequestCB m_restfulRequestCB;    ///< 业务回调(NetDock 注入,Open 前设置)
};

#endif // HTTP_RESTFUL_MANAGER_H
