#ifndef NET_DOCK_H
#define NET_DOCK_H

#include "dock_runloop.h"
#include "message_server_manager.h"
#include "http_jsonrpc_manager.h"

/**
 * @brief 网络层生命周期编排者
 *
 * 持有事件循环线程和各个网络服务器管理器，负责：
 *   1. 创建并启动 libevent 事件循环（DockRunLoop）
 *   2. 编排 WebSocket 服务器和 HTTP JSON-RPC 管道的启停
 *   3. 提供跨线程任务调度接口（ScheduleTaskInLoop）
 */
class NetDock
{
public:
    NetDock();
    ~NetDock();

    /** @brief 初始化事件循环线程，阻塞直到 loop 就绪 */
    void Init();

    /** @brief 启动 WebSocket 服务器 */
    void OpenWebSocketServer();
    /** @brief 停止 WebSocket 服务器 */
    void CloseWebSocketServer();

    /** @brief 启动 HTTP JSON-RPC 管道 */
    void OpenHttpServer();
    /** @brief 停止 HTTP JSON-RPC 管道 */
    void CloseHttpServer();

    /** @brief 设置 JRPC 请求的外部回调（需在 OpenHttpServer 之前调用） */
    void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

    /** @brief 调度任务到 libevent 主线程执行
     *  @param fn_run 在 libevent 线程中执行的任务
     *  @param fn_cb  任务执行后的回调
     *  @param param  透传参数
     *  @return true 调度成功，false 事件循环未就绪 */
    bool ScheduleTaskInLoop(std::function<void(void*)> fn_run,
                            std::function<void(void*)> fn_cb, void* param);

private:
    /** @brief 调度事件回调，在 libevent 线程中执行 fn_run → fn_cb 后释放 ctx */
    static void OnScheduleEventCB(evutil_socket_t fd, short what, void* ctx);

    DockRunLoop*           m_dockRunloop;         ///< libevent 事件循环线程
    event_base*            m_evbase;              ///< 缓存的 event_base（用于 ScheduleTaskInLoop）
    MessageServerManager*  m_messageServerMgr;    ///< WebSocket 服务器管理器
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 管道管理器
    TapDelegateJrpcRequestReadCB m_jrpcRequestReadCB;  ///< JRPC 外部回调，OpenHttpServer 时注入管道
};

#endif // NET_DOCK_H
