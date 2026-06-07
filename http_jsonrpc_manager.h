#ifndef HTTP_JSONRPC_MANAGER_H
#define HTTP_JSONRPC_MANAGER_H

#include "zm_net_tap.h"
#include "zm_net_tap_hub.h"
#include "zm_net_tap_jrpc.h"
#include "zm_net_http.h"

/**
 * @brief HTTP JSON-RPC 管道管理器
 *
 * 管理从 HTTP 服务器到 TAP Hub 代理的完整请求处理管道：
 *   ZmJsonRpcServer → OnHttpJsonrpcEx → SendToHubProxy（TCP 短连接）
 *     → ZmTapHubProxy → ZmTapDelegateJRPC → 外部回调
 */
class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager();
    ~HttpJsonRpcManager();

    /**
     * @brief 初始化管道：创建 TAP 组件链并启动 HTTP 服务器
     * @param evbase     libevent 事件循环基
     * @param evdnsbase  libevent DNS 解析基
     * @param cb         JRPC 请求到达时的外部回调
     * @return 是否全部初始化成功
     */
    bool Open(event_base* evbase, evdns_base* evdnsbase,
              TapDelegateJrpcRequestReadCB cb);

    /** @brief 关闭管道：释放 HTTP 服务器和所有 TAP 组件 */
    void Close();

    /** @brief 获取 Hub 代理端口（0 表示未就绪），供外部建立 TCP 连接使用 */
    uint16_t HubProxyPort() const { return m_hubProxyPort; }

private:
    /** @brief HTTP JSON-RPC 请求回调，内部调用 SendToHubProxy 转发到 TAP 管道 */
    int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
                        const ZMJSON& params, ZMJSON& result, ZMJSON& error);

    /** @brief 通过 TCP 短连接向本地 Hub Proxy 发送 JRPC 请求并等待响应
     *  @param reqjs  请求 JSON 字符串
     *  @param rspjs  输出：响应 JSON 字符串
     *  @return true 成功，false 失败 */
    bool SendToHubProxy(const char* reqjs, std::string& rspjs);

    // --- 成员变量 ---
    ZmTapContext*       m_tapContext;        ///< TAP 上下文池
    ZmTapDelegateJRPC*  m_tapDelegateJRPC;   ///< JRPC 协议委托处理器
    ZmTapHubProxy*      m_tapHubProxy;       ///< Hub 代理
    ZmJsonRpcServer*    m_httpServerJRPC;    ///< HTTP JSON-RPC 服务器
    uint16_t            m_hubProxyPort;      ///< Hub 代理监听端口（0 表示未就绪）

    event_base*         m_evbase;            ///< 外部传入的 libevent 事件循环基
    evdns_base*         m_evdnsbase;         ///< 外部传入的 libevent DNS 解析基
};

#endif // HTTP_JSONRPC_MANAGER_H
