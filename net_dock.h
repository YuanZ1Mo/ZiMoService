#ifndef NET_DOCK_H
#define NET_DOCK_H

#include "dock_runloop.h"
#include "message_server_manager.h"
#include "hub_proxy_manager.h"
#include "http_jsonrpc_manager.h"

/**
 * @brief 网络层生命周期编排者
 *
 * 持有事件循环线程和各个网络服务器管理器，负责：
 *   1. 创建并启动 libevent 事件循环（DockRunLoop）
 *   2. 编排 WebSocket / Hub / HTTP JRPC / HTTP SOCKS5 的启停
 *   3. 提供跨线程任务调度接口（ScheduleTaskInLoop）
 *
 * 启动顺序约束：
 *   Init → OpenHub → OpenHttpJsonRpcServer / OpenSocks5Server（Hub 必须先于前端启动）
 */
class NetDock
{
public:
    NetDock();
    ~NetDock();

    /** @brief 初始化事件循环线程，阻塞直到 loop 就绪 */
    void Init();

    /** @brief 反初始化：按正确顺序关闭所有组件（可多次调用，析构时自动调用） */
    void UnInit();

    /** @brief 启动 WebSocket 服务器 */
    void OpenWebSocketServer();
    /** @brief 停止 WebSocket 服务器 */
    void CloseWebSocketServer();

    /** @brief 启动 TAP Hub 路由层（多协议前端共享，需在 OpenHttpJsonRpcServer 之前调用） */
    void OpenHub();
    /** @brief 停止 TAP Hub 路由层 */
    void CloseHub();

    /** @brief 启动 HTTP JSON-RPC 前端（依赖 Hub 已启动） */
    void OpenHttpJsonRpcServer();
    /** @brief 停止 HTTP JSON-RPC 前端 */
    void CloseHttpJsonRpcServer();

    /** @brief 预留 SOCKS5 入口（依赖 Hub 已启动） */
    void OpenSocks5Server();
    /** @brief 预留 停止 SOCKS5 */
    void CloseSocks5Server();

    /** @brief 设置 JRPC 请求的外部回调（需在 OpenHub 之前调用） */
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
    HubProxyManager*       m_hubProxyMgr;         ///< TAP Hub 路由层（多协议前端共享）
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 前端
    TapDelegateJrpcRequestReadCB m_jrpcRequestReadCB;  ///< JRPC 外部回调，OpenHub 时注入
    bool                              m_unInited;            ///< 防止 UnInit 重复执行
};

#endif // NET_DOCK_H
