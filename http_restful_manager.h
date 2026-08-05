#ifndef HTTP_RESTFUL_MANAGER_H
#define HTTP_RESTFUL_MANAGER_H

#include "zm_net_http.h"
#include "zm_net_req_loop.h"   // ZmReqLoopRestfulRequestCB(业务回调类型)

// 前向声明（头文件中仅通过指针使用）
class ZmEvBaseRunLoop;

/**
 * @brief HTTP RESTful 前端管理器
 *
 * 负责 ZmRESTfulServer 的自有事件循环(ZmEvBaseRunLoop)生命周期管理。
 * ZmReqLoopPool 已下沉到 ZmRESTfulServer 内部自治
 * (EnableLoopPool/AcquireLoop/DispatchLoop,见 zm_net_http.h),
 * 请求分发(前缀过滤 → ZmReqLoopPool 获取 → START 投递)全部由服务器类完成,
 * 本管理器仅保留:evLoop 创建/停止、证书与 ticket 转发、业务回调透传。
 *
 * 异步处理流程:
 *   ① HTTP 请求到达 Worker 线程 → 服务器 OnHttpdRequest 前缀过滤
 *   ② 内部 ZmReqLoopPool Acquire(排队/超时→503,deadline 超时→504)
 *   ③ task->BindLoop → PostToLoop(START) → ZmReqLoop 线程:业务回调(loop, body, len)
 *   ④ 业务在 ZmReqLoop 线程分段推进 → ZmReqLoopRest::Response* 直通 task 回复
 *   ⑤ 回复 helper 内部:TryReply 门 + Release → 回池
 */
class HttpRestfulManager
{
public:
    HttpRestfulManager();
    ~HttpRestfulManager();

    /**
     * @brief 初始化 HTTP/HTTPS RESTful 服务器(含内部 ZmReqLoopPool)
     * @return true 初始化成功
     */
    bool Open();

    /** @brief 关闭 HTTP 服务器、ZmReqLoopPool 和自有事件循环(软关闭) */
    void Close();

    /** @brief 设置 RESTful 业务回调(NetDock 在 Open 前调用,Open 时透传到服务器) */
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
    ZmEvBaseRunLoop*            m_evLoopHttpServer;   ///< 自有事件循环线程(供 ZmRESTfulServer 使用)
    ZmRESTfulServer*            m_httpServerRESTful;  ///< RESTful 服务器实例(含内部 ZmReqLoopPool)
    ZmReqLoopRestfulRequestCB   m_restfulRequestCB;   ///< 业务回调(NetDock 注入,Open 前设置)
};

#endif // HTTP_RESTFUL_MANAGER_H
