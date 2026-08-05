#ifndef NET_DOCK_H
#define NET_DOCK_H

#include <cstdint>
#include <functional>

#include "zm_json.h"
#include "zm_util_str.h"
#include "zm_net_req_loop.h"

// 前向声明（头文件中仅通过指针/引用使用）
class HttpJsonRpcManager;
class HttpRestfulManager;
class HttpServerManager;
class BroadcastManager;
class ZmHttpRouter;
class ZmTicketKeyRotator;

/**
 * @brief 网络层生命周期编排者
 *
 * 创建并持有各个网络服务器管理器，负责：
 *   1. HttpJsonRpcManager — HTTP JSON-RPC 前端（含私有 ZmReqLoopPool，端口 39440）
 *   2. HttpRestfulManager — HTTP RESTful 前端（含私有 ZmReqLoopPool，端口 39441）
 *   3. HttpServerManager — 通用 HTTP 前端（端口 80）
 *   4. BroadcastManager — 广播服务端（端口 39640，消息推送）
 *
 * 请求链：HTTP → doer → 各 manager 私有 ZmReqLoopPool（Acquire 排队）→ 业务回调（ZmReqLoop 线程）
 * → ZmReqLoopJrpc::ResponseJson / ZmReqLoopRest::Response 直通 task 发送（不绕 Hub/pair）。
 *
 * 启动顺序约束：
 *   Init → 注入业务回调（SetJrpcRequestReadCB/SetRESTfulRequestCB，须在 Open 前）
 *   → OpenHttpJsonRpcServer / OpenHttpRESTfulServer / OpenHttpServer / OpenBroadcastServer
 *
 * 关闭顺序约束：
 *   ① HTTP 前端软关闭 — Close() 停 HTTP Server + 排空 worker + 停 ZmReqLoopPool
 *      （在飞请求由各自 deadline 收尾）
 *   ② HttpJsonRpcManager / HttpRestfulManager delete — 析构兜底（幂等）
 */
class NetDock
{
public:
    NetDock();
    ~NetDock();

    /**
     * @brief 初始化 WinSock 环境
     * @note 重复调用安全
     */
    void Init();

    /**
     * @brief 反初始化：按正确顺序关闭所有组件
     * @note 可多次调用（幂等），析构时自动调用
     */
    void UnInit();

    // --- HTTP 前端 ---

    /**
     * @brief 启动 HTTP JSON-RPC 前端
     *
     * 内部创建 HttpJsonRpcManager（含私有 ZmReqLoopPool）；业务回调在 Open 前注入。
     */
    void OpenHttpJsonRpcServer();
    /** @brief 停止 HTTP JSON-RPC 前端（软关闭：停 HTTP Server + 排空 worker + 停 ZmReqLoopPool） */
    void CloseHttpJsonRpcServer();

    /**
     * @brief 启动 HTTP RESTful 前端（端口独立）
     */
    void OpenHttpRESTfulServer();
    /** @brief 停止 HTTP RESTful 前端 */
    void CloseHttpRESTfulServer();

    /**
     * @brief 启动通用 HTTP 服务器（wwwRoot/证书路径由 HttpServerManager 内部推导）
     * @note HTTPS 时：443 端口 + 80→443 重定向；HTTP 时：仅 80 端口
     */
    void OpenHttpServer();
    /** @brief 停止通用 HTTP 服务器 */
    void CloseHttpServer();

    // --- TLS 基础设施 ---

    /**
     * @brief 确保 TLS session ticket 轮换器已创建并启动(幂等,懒创建)
     * @note 仅 HTTPS 模式调用;纯 HTTP 模式不创建,零开销
     * @note 仅在主线程启动路径调用,无并发保护
     */
    ZmTicketKeyRotator* EnsureTicketRotator();
    /** @brief ticket 密钥轮换完成回调:投递到各 HTTPS 服务器事件循环 */
    void OnTicketRotated();

    /** @brief 预留 SOCKS5 入口 */
    void OpenSocks5Server();
    /** @brief 预留 停止 SOCKS5 */
    void CloseSocks5Server();

    // --- 广播服务端 ---

    /**
     * @brief 启动广播服务端
     * @param port 监听端口，0 = 随机分配
     */
    void OpenBroadcastServer();
    /** @brief 停止广播服务端 */
    void CloseBroadcastServer();
    /** @brief 获取广播服务端管理器指针 */
    BroadcastManager* GetBroadcastManager();
    /** @brief 广播服务端是否运行中 */
    bool IsBroadcastOpen() const;

    /**
     * @brief 获取通用 HTTP 路由器的引用，供业务层注册 API 端点
     * @return ZmHttpRouter& 路由器引用
     * @note 需在 OpenHttpServer 之后调用
     */
    ZmHttpRouter& GetHttpRouter();

    /**
     * @brief 获取 HTTP 服务器管理器指针，供业务层注册静态文件路由
     * @return HttpServerManager* 指针，未初始化时返回 nullptr
     */
    HttpServerManager* GetHttpServerManager();

    /** @brief 获取 RESTful HTTP 管理器指针 */
    HttpRestfulManager* GetHttpRestfulManager() { return m_httpRestfulMgr; }

    // --- 状态查询 ---

    /** @brief HTTP 服务器是否运行中 */
    bool IsHttpOpen() const;
    /** @brief JRPC HTTP 服务器是否运行中 */
    bool IsJrpcHttpOpen() const;
    /** @brief RESTful HTTP 服务器是否运行中 */
    bool IsRESTfulHttpOpen() const;

    // --- 回调设置 ---

    /**
     * @brief 设置 JRPC 请求的外部回调
     * @param cb 回调函数，参数为 A 实例(ZmReqLoop*)和请求数据
     * @note 需在 OpenHttpJsonRpcServer 之前调用
     */
    void SetJrpcRequestReadCB(ZmReqLoopJrpcRequestCB cb);
    void SetRESTfulRequestCB(ZmReqLoopRestfulRequestCB cb);

private:
    // --- 成员变量 ---
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 前端（含私有 ZmReqLoopPool）
    HttpRestfulManager*    m_httpRestfulMgr;      ///< HTTP RESTful 前端（含私有 ZmReqLoopPool）
    HttpServerManager*     m_httpServerMgr;       ///< 通用 HTTP 前端（端口 80）
    BroadcastManager*      m_broadcastMgr;        ///< 广播服务端管理器
    ZmTicketKeyRotator*    m_ticketRotator;       ///< TLS ticket 密钥轮换器(懒创建,UnInit 释放)
    ZmReqLoopJrpcRequestCB m_jrpcRequestReadCB;   ///< JRPC 业务回调(Open 前注入)
    ZmReqLoopRestfulRequestCB m_restfulRequestCB; ///< RESTful 业务回调(Open 前注入)
    bool                   m_unInited;            ///< 防止 UnInit 重复执行
};

#endif // NET_DOCK_H
