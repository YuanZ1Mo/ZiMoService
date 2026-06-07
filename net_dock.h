#ifndef NET_DOCK_H
#define NET_DOCK_H

#include "dock_runloop.h"
#include "message_server_manager.h"
#include "hub_proxy_manager.h"
#include "http_jsonrpc_manager.h"

#include <mutex>
#include <unordered_set>

/**
 * @brief 网络层生命周期编排者
 *
 * 创建并持有事件循环线程和各个网络服务器管理器，负责：
 *   1. DockRunLoop — libevent 事件循环线程
 *   2. MessageServerManager — WebSocket 服务器
 *   3. HubProxyManager — TAP Hub 路由层（多协议前端共享）
 *   4. HttpJsonRpcManager — HTTP JSON-RPC 前端
 *   5. ScheduleTaskInLoop — 跨线程任务调度
 *
 * 启动顺序约束：
 *   Init → OpenHub → OpenHttpJsonRpcServer / OpenSocks5Server（Hub 必须先于前端启动）
 *
 * 关闭顺序约束：
 *   前端先停 → Hub 再停 → DockRunLoop 最后停
 */
class NetDock
{
public:
    NetDock();
    ~NetDock();

    /**
     * @brief 初始化：启动 DockRunLoop 事件循环线程，阻塞直到 loop 就绪
     * @note 重复调用安全
     */
    void Init();

    /**
     * @brief 反初始化：按正确顺序关闭所有组件
     * @note 可多次调用（幂等），析构时自动调用
     */
    void UnInit();

    // --- WebSocket 服务器 ---

    /** @brief 启动 WebSocket 服务器 */
    void OpenWebSocketServer();
    /** @brief 停止 WebSocket 服务器 */
    void CloseWebSocketServer();

    // --- Hub 路由层 ---

    /**
     * @brief 启动 TAP Hub 路由层（多协议前端共享）
     * @note 需在 OpenHttpJsonRpcServer / OpenSocks5Server 之前调用
     */
    void OpenHub();
    /** @brief 停止 TAP Hub 路由层 */
    void CloseHub();

    // --- HTTP 前端 ---

    /**
     * @brief 启动 HTTP JSON-RPC 前端
     * @note 依赖 Hub 已启动，否则仅输出错误日志而不创建 HTTP 服务器
     */
    void OpenHttpJsonRpcServer();
    /** @brief 停止 HTTP JSON-RPC 前端 */
    void CloseHttpJsonRpcServer();

    /** @brief 预留 SOCKS5 入口（依赖 Hub 已启动） */
    void OpenSocks5Server();
    /** @brief 预留 停止 SOCKS5 */
    void CloseSocks5Server();

    // --- 回调设置 ---

    /**
     * @brief 设置 JRPC 请求的外部回调
     * @param cb 回调函数，参数为 TAP 上下文和请求数据
     * @note 需在 OpenHub 之前调用
     */
    void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

    /**
     * @brief 调度任务到 libevent 主线程执行
     * @param fn_run 在 libevent 线程中执行的任务
     * @param fn_cb  任务执行后的回调
     * @param param  透传参数
     * @return true 调度成功，false 事件循环未就绪
     *
     * 通过 libevent 的 event_new + event_active 实现跨线程调遣。
     * ctx 被记录到 m_pendingScheduleCtx，UnInit 时会清理未触发的残留。
     */
    bool ScheduleTaskInLoop(std::function<void(void*)> fn_run,
                            std::function<void(void*)> fn_cb, void* param);

private:
    /** @brief 调度事件回调，在 libevent 线程中执行 fn_run → fn_cb 后释放 ctx */
    static void OnScheduleEventCB(evutil_socket_t fd, short what, void* ctx);

    // --- 调度任务上下文 ---

    /**
     * @brief 调度任务上下文，event_new → OnScheduleEventCB → delete
     *
     * owner 字段（NetDock*）使静态回调能访问 m_pendingScheduleCtx 集合，
     * 用于在回调中移除自身和在 UnInit 中兜底清理。
     */
    struct ZM_SCHEDULE_CTX
    {
        event*                      ev_schedule;     ///< libevent 调度事件
        std::function<void(void*)>  fn_run;          ///< 在事件循环线程执行的任务
        std::function<void(void*)>  fn_cb;           ///< 任务完成后的回调
        void*                       param;           ///< 透传参数
        void*                       owner;           ///< NetDock*，用于回调中访问 pending 集合
    };

    // --- 成员变量 ---
    DockRunLoop*           m_dockRunloop;         ///< libevent 事件循环线程
    event_base*            m_evbase;              ///< 缓存的 event_base（从 DockRunLoop 获取）
    MessageServerManager*  m_messageServerMgr;    ///< WebSocket 服务器管理器
    HubProxyManager*       m_hubProxyMgr;         ///< TAP Hub 路由层（多协议前端共享）
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 前端
    TapDelegateJrpcRequestReadCB m_jrpcRequestReadCB;  ///< JRPC 外部回调，OpenHub 时注入
    bool                   m_unInited;            ///< 防止 UnInit 重复执行

    std::mutex                    m_scheduleMutex;       ///< 保护 m_pendingScheduleCtx
    std::unordered_set<ZM_SCHEDULE_CTX*> m_pendingScheduleCtx;  ///< 未触发的调度任务
};

#endif // NET_DOCK_H
