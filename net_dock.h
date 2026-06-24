#ifndef NET_DOCK_H
#define NET_DOCK_H

#include <cstdint>
#include <functional>

// 前向声明（头文件中仅通过指针/引用使用）
class HubProxyManager;
class HttpJsonRpcManager;
class HttpServerManager;
class BroadcastManager;
class ZmHttpRouter;
struct ZM_TAP_CTX;

// TapDelegateJrpcRequestReadCB using 别名
// （原始定义位于 zm_net_tap_jrpc.h，此处复制以避免拉入完整 zm_net_tap.h 链）
using TapDelegateJrpcRequestReadCB = std::function<void(struct ZM_TAP_CTX*, const char*)>;

/**
 * @brief 网络层生命周期编排者
 *
 * 创建并持有各个网络服务器管理器，负责：
 *   1. HubProxyManager — TAP Hub 路由层（内部持有 ZmEvBaseRunLoop 事件循环线程）
 *   2. HttpJsonRpcManager — HTTP JSON-RPC 前端（含内部 JRPC 请求通道）
 *   3. HttpServerManager — 通用 HTTP 前端（端口 80）
 *   4. BroadcastManager — 广播服务端（端口 39640，消息推送）
 *
 * 跨线程 TAP 操作（Response/SetDropTimer/Drop）
 * 已迁移到 ZmTapContext（静态方法），业务层直接通过 ZmTapContext:: 调用。
 *
 * 启动顺序约束：
 *   Init → OpenHub → OpenHttpJsonRpcServer（内部自行创建 ZmNetRequestChannel）
 *
 * 关闭顺序约束：
 *   ① HTTP 前端软关闭 — Close() 停 HTTP Server + 线程池（Pair 池保留给在飞请求）
 *   ② Hub 停 — StopThreadPool → 清所有 TAP（pair1 EOF → pair 归还池）
 *              → beforeLoopStop 回调销毁 pair 池 → 停 event loop
 *   ③ HttpJsonRpcManager delete — 析构兜底（pair 池已在步骤②中销毁，跳过）
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

    /** @brief 预留 SOCKS5 入口（依赖 Hub 已启动） */
    void OpenSocks5Server();
    /** @brief 预留 停止 SOCKS5 */
    void CloseSocks5Server();

    // --- 广播服务端 ---

    /**
     * @brief 启动广播服务端（依赖 Hub 已启动，内部使用 Hub 的事件循环线程）
     * @param port 监听端口，0 = 随机分配
     */
    void OpenBroadcastServer(uint16_t port);
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

    // --- 状态查询 ---

    /** @brief HTTP 服务器是否运行中 */
    bool IsHttpOpen() const;
    /** @brief JRPC HTTP 服务器是否运行中 */
    bool IsJrpcHttpOpen() const;
    /** @brief Hub 路由层是否运行中 */
    bool IsHubOpen() const;
    /** @brief JRPC Proxy delegate 是否运行中 */
    bool IsJrpcProxyOpen() const;

    // --- 回调设置 ---

    /**
     * @brief 设置 JRPC 请求的外部回调
     * @param cb 回调函数，参数为 TAP 上下文和请求数据
     * @note 需在 OpenHub 之前调用
     */
    void SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb);

private:
    // --- 成员变量 ---
    HubProxyManager*       m_hubProxyMgr;         ///< TAP Hub 路由层（多协议前端共享，内部持有 ZmEvBaseRunLoop）
    HttpJsonRpcManager*    m_httpJsonRpcMgr;      ///< HTTP JSON-RPC 前端（含内部请求通道）
    HttpServerManager*     m_httpServerMgr;       ///< 通用 HTTP 前端（端口 80）
    BroadcastManager*      m_broadcastMgr;        ///< 广播服务端管理器
    TapDelegateJrpcRequestReadCB m_jrpcRequestReadCB;  ///< JRPC 外部回调，OpenHub 时注入
    bool                   m_unInited;            ///< 防止 UnInit 重复执行
};

#endif // NET_DOCK_H
