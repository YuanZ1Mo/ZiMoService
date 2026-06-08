#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_http.h"
#include "hub_proxy_manager.h"

/**
 * @brief HTTP JSON-RPC 前端管理器
 *
 * 仅负责 HTTP JSON-RPC 服务器的生命周期，请求通过 bufferevent pair
 * 交付给 HubProxyManager → HubProxy → JRPC Delegate 处理。
 *
 * 依赖关系：
 *   HttpJsonRpcManager → HubProxyManager → ZmTapHubProxy → ZmTapDelegateJRPC
 */
class HttpJsonRpcManager
{
public:
    /**
     * @brief 在事件循环线程中执行任务的回调类型
     *
     * 签名与 NetDock::ScheduleTaskInLoop 一致：
     *   @param fn_run 在事件循环线程执行的任务
     *   @param fn_cb  任务完成后的回调
     *   @param param  透传参数
     *   @return true 调度成功，false 事件循环未就绪
     */
    using ScheduleFn = std::function<bool(std::function<void(void*)>, std::function<void(void*)>, void*)>;

    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP JSON-RPC 服务器
     * @param hubMgr  已初始化的 Hub 路由层管理器（不持有所有权）
     * @return true 初始化成功
     */
    bool Open(HubProxyManager* hubMgr);

    /**
     * @brief 关闭 HTTP 服务器（停止 ZmJsonRpcServer 线程并释放资源）
     */
    void Close();

    /**
     * @brief 设置事件循环线程调度回调
     * @param fn 调度函数，签名与 NetDock::ScheduleTaskInLoop 一致
     *
     * 必须在 Open 之前调用，供 SendToHubProxy 跨线程交付 bufferevent pair
     */
    void SetScheduleFn(ScheduleFn fn) { m_scheduleFn = fn; }

    /** @brief 查询 JRPC 服务器是否已启动 */
    bool IsOpen() const { return m_httpServerJRPC != nullptr; }

private:
    /**
     * @brief 通过 bufferevent pair 向 Hub Proxy 交付 JRPC 请求并等待响应
     * @param reqjs  请求 JSON 字符串
     * @param rspjs  输出：响应 JSON 字符串
     * @return true 成功，false 失败
     *
     * 使用 evutil_socketpair 创建内存级互联 socket 对，Worker 线程端写入请求、
     * 阻塞等待响应；事件循环线程端通过 OnPairAcceptConn 注入 Hub Proxy。
     */
    bool SendToHubProxy(const char* reqjs, std::string& rspjs);

    /**
     * @brief HTTP JSON-RPC 请求回调，由 ZmHttpdDoer Worker 线程调用
     * @param task   请求上下文对象
     * @param method JRPC 方法名
     * @param params JRPC 参数
     * @param result 输出：JRPC 成功结果
     * @param error  输出：JRPC 错误信息
     * @return HTTP 状态码（0 表示成功）
     */
    int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
                        const ZMJSON& params, ZMJSON& result, ZMJSON& error);

    // --- 成员变量 ---
    ZmJsonRpcServer*    m_httpServerJRPC;    ///< HTTP JSON-RPC 服务器
    HubProxyManager*    m_hubMgr;            ///< Hub 路由层（外部注入，不持有所有权）
    ScheduleFn          m_scheduleFn;        ///< 事件循环线程调度回调
};

#endif // HTTP_JSONRPC_MANAGER_H
