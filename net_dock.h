#ifndef NET_DOCK_H
#define NET_DOCK_H

#include "dock_runloop.h"
#include "message_server_manager.h"
#include "hub_proxy_manager.h"
#include "http_jsonrpc_manager.h"
#include "http_server_manager.h"

#include <mutex>
#include <unordered_set>

/**
 * @brief 网络层生命周期编排者
 *
 * 创建并持有事件循环线程和各个网络服务器管理器，负责：
 *   1. DockRunLoop — libevent 事件循环线程
 *   2. MessageServerManager — WebSocket 服务器
 *   3. HubProxyManager — TAP Hub 路由层（多协议前端共享）
 *   4. HttpJsonRpcManager — HTTP JSON-RPC 前端（含内部 JRPC 请求通道）
 *   5. ScheduleTaskInLoop — 跨线程任务调度
 *
 * 启动顺序约束：
 *   Init → OpenHub → OpenHttpJsonRpcServer（内部自行创建 ZmNetRequestChannel）
 *
 * 关闭顺序约束：
 *   前端先停（内部先关 ZmNetRequestChannel 再 join Worker）→ Hub 停 → DockRunLoop 停
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
     *
     * HttpJsonRpcManager 内部自行创建 ZmNetRequestChannel 并将请求
     * 通过 bufferevent_pair 注入 Hub 代理链。
     */
    void OpenHttpJsonRpcServer();
    /** @brief 停止 HTTP JSON-RPC 前端（内部先关通道再 join Worker） */
    void CloseHttpJsonRpcServer();

    /**
     * @brief 启动通用 HTTP 服务器（端口 80，不依赖 Hub/JRPC）
     * @param wwwRoot 静态文件根目录路径（绝对路径），为空不启用静态文件
     */
    void OpenHttpServer(const char* wwwRoot = nullptr);
    /** @brief 停止通用 HTTP 服务器 */
    void CloseHttpServer();

    /**
     * @brief 获取通用 HTTP 路由器的引用，供业务层注册 API 端点
     * @return ZmHttpRouter& 路由器引用
     * @note 需在 OpenHttpServer 之后调用
     */
    ZmHttpRouter& GetHttpRouter();

    // --- 状态查询 ---

    /** @brief HTTP 服务器是否运行中 */
    bool IsHttpOpen() const;
    /** @brief JRPC HTTP 服务器是否运行中 */
    bool IsJrpcHttpOpen() const;
    /** @brief Hub 路由层是否运行中 */
    bool IsHubOpen() const;
    /** @brief JRPC Proxy delegate 是否运行中 */
    bool IsJrpcProxyOpen() const;
    /** @brief WebSocket 服务器是否运行中 */
    bool IsWebSocketOpen() const;

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

    // --- TAP 通用响应操作（必须在 libevent 线程中调用）---

    /**
     * @brief 通过 TAP 回传链写入 JSON 响应（必须在 libevent 线程中调用）
     * @param tap        请求关联的 TAP 上下文
     * @param jsResponse 完整响应 JSON（业务层自行封装 {"result":...} 或 {"error":...} 外层）
     *
     * 从 TAP 回传链弹出 JRPC delegate，设置回传数据后触发 OnTapDelegateBackEvent
     * 将响应通过长度前缀帧写回客户端
     */
    void Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);

    // --- TAP 通用操作（可在任意线程中调用，内部回投到 libevent 线程）---

    /**
     * @brief 异步写入 JSON 响应（可在任意线程中调用）
     * @param tap        请求关联的 TAP 上下文
     * @param jsResponse 完整响应 JSON（业务层自行封装 {"result":...} 或 {"error":...} 外层）
     *
     * 内部通过 ScheduleTaskInLoop 将实际写入回投到 libevent 线程，
     * 回投时会检查 TAP 状态（已 Drop 则丢弃响应并输出警告日志）。
     * JSON 在回投前序列化为字符串以避免跨线程访问 nlohmann::json。
     */
    void ResponseAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);

    /**
     * @brief 异步设置 TAP 超时定时器（可在任意线程中调用）
     * @param tap                     TAP 上下文
     * @param seconds                 超时秒数（传负值取消定时器）
     * @param micros                  超时微秒数（传负值取消定时器）
     * @param drop_timeout_error_code 超时错误码，到期未取消则触发 Drop
     *
     * 内部通过 ScheduleTaskInLoop 将 event_add/evtimer_del 回投到 libevent 线程，
     * 避免跨线程操作 event_base 导致的竞态条件
     */
    void SetDropTimerAsync(ZM_TAP_CTX* tap, int seconds, int micros = 0, uint32_t drop_timeout_error_code = 0);

    /**
     * @brief 异步释放 TAP（可在任意线程中调用）
     * @param tap    TAP 上下文
     * @param reason 释放原因（用于日志追踪）
     *
     * 内部通过 ScheduleTaskInLoop 将 Drop 回投到 libevent 线程，
     * 避免跨线程操作 bufferevent / event 导致的竞态条件。
     */
    void DropAsync(ZM_TAP_CTX* tap, const char* reason);

private:
    /** @brief 调度事件回调，在 libevent 线程中执行 fn_run → fn_cb 后释放 ctx */
    static void OnScheduleEventCB(evutil_socket_t fd, short what, void* ctx);

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
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 前端（含内部请求通道）
    HttpServerManager*     m_httpServerMgr;       ///< 通用 HTTP 前端（端口 80）
    TapDelegateJrpcRequestReadCB m_jrpcRequestReadCB;  ///< JRPC 外部回调，OpenHub 时注入
    bool                   m_unInited;            ///< 防止 UnInit 重复执行

    std::mutex                    m_scheduleMutex;       ///< 保护 m_pendingScheduleCtx
    std::unordered_set<ZM_SCHEDULE_CTX*> m_pendingScheduleCtx;  ///< 未触发的调度任务
};

#endif // NET_DOCK_H
