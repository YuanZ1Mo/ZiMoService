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
    /** @brief 在事件循环线程中执行任务的回调类型 */
    using ScheduleFn = std::function<bool(std::function<void(void*)>, std::function<void(void*)>, void*)>;

    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化 HTTP JSON-RPC 服务器
     * @param hubMgr  已初始化的 Hub 路由层管理器
     * @return 是否全部初始化成功
     */
    bool Open(HubProxyManager* hubMgr);

    /** @brief 关闭 HTTP 服务器 */
    void Close();

    /** @brief 设置事件循环线程调度回调
     *  @param fn 调度函数，签名与 NetDock::ScheduleTaskInLoop 一致 */
    void SetScheduleFn(ScheduleFn fn) { m_scheduleFn = fn; }

private:
    /** @brief HTTP JSON-RPC 请求回调，内部调用 SendToHubProxy 转发到 Hub 路由层 */
    int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
                        const ZMJSON& params, ZMJSON& result, ZMJSON& error);

    /** @brief 通过 bufferevent pair 向 Hub Proxy 交付 JRPC 请求并等待响应
     *  @param reqjs  请求 JSON 字符串
     *  @param rspjs  输出：响应 JSON 字符串
     *  @return true 成功，false 失败 */
    bool SendToHubProxy(const char* reqjs, std::string& rspjs);

    // --- 成员变量 ---
    ZmJsonRpcServer*    m_httpServerJRPC;    ///< HTTP JSON-RPC 服务器
    HubProxyManager*    m_hubMgr;            ///< Hub 路由层（外部注入，不持有所有权）
    ScheduleFn          m_scheduleFn;        ///< 事件循环线程调度回调
};

#endif // HTTP_JSONRPC_MANAGER_H
