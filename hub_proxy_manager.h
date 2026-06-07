#ifndef HUB_PROXY_MANAGER_H
#define HUB_PROXY_MANAGER_H

#include "zm_net_tap.h"
#include "zm_net_tap_hub.h"
#include "zm_net_tap_jrpc.h"

/**
 * @brief TAP Hub 路由层管理器，为多个协议前端提供共享的消息路由能力
 *
 * 生命周期：
 *   Open()  → 创建 TAP 上下文池、Hub Proxy、JRPC Delegate（未来可扩展 SOCKS5 等）
 *   Close() → 逆序释放所有组件
 *
 * 协议前端（HttpJsonRpcManager / HttpSocks5Manager）通过 OnPairAcceptConn()
 * 将请求注入 Hub Proxy，由协议探测自动分发到对应 delegate。
 */
class HubProxyManager
{
public:
    HubProxyManager();
    ~HubProxyManager();

    /**
     * @brief 初始化 Hub 路由层
     * @param evbase      libevent 事件循环基
     * @param evdnsbase   libevent DNS 解析基
     * @param jrpcCB      JRPC 请求到达时的外部回调
     * @return 是否全部初始化成功
     */
    bool Open(event_base* evbase, evdns_base* evdnsbase,
              TapDelegateJrpcRequestReadCB jrpcCB);

    /** @brief 关闭路由层，逆序释放所有 TAP 组件 */
    void Close();

    // --- 供协议前端访问的只读接口 ---
    event_base*      EvBase()     const { return m_evbase; }
    ZmTapContext*    TapContext() const { return m_tapContext; }
    ZmTapHubProxy*   HubProxy()   const { return m_tapHubProxy; }

private:
    ZmTapContext*      m_tapContext;       ///< TAP 上下文池（所有前端共享）
    ZmTapDelegateJRPC* m_tapDelegateJRPC;  ///< JRPC 协议委托处理器
    ZmTapHubProxy*     m_tapHubProxy;      ///< Hub 代理（共享消息路由）
    uint16_t           m_hubProxyPort;     ///< Hub 代理监听端口（兼容旧 TCP 路径）

    event_base*        m_evbase;           ///< libevent 事件循环基
    evdns_base*        m_evdnsbase;        ///< libevent DNS 解析基
};

#endif // HUB_PROXY_MANAGER_H
