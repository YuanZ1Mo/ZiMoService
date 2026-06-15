#ifndef HUB_PROXY_MANAGER_H
#define HUB_PROXY_MANAGER_H

#include "zm_net_tap.h"
#include "zm_net_tap_hub.h"
#include "zm_net_tap_jrpc.h"
#include "zm_net_runloop.h"

/**
 * @brief TAP Hub 路由层管理器，为多个协议前端提供共享的消息路由能力
 *
 * 内部持有 ZmEvBaseRunLoop（libevent 事件循环线程），所有 TAP delegate
 * 共享同一条事件循环。协议前端通过 EvBase() / EvDnsBase() 获取事件循环引用。
 *
 * 生命周期：
 *   Open()  → 启动 ZmEvBaseRunLoop → 创建 TAP 上下文池、Hub Proxy、JRPC Delegate
 *   Close() → TAP 上下文清理 → HubProxy delegate 停止 → JRPC delegate 停止
 *          → ZmEvBaseRunLoop 停止
 *
 * 协议前端（HttpJsonRpcManager）通过 OnPairAcceptConn()
 * 将请求注入 Hub Proxy，由协议探测自动分发到对应 delegate。
 */
class HubProxyManager
{
public:
    HubProxyManager();
    ~HubProxyManager();

    /**
     * @brief 初始化 Hub 路由层（内部创建并启动 ZmEvBaseRunLoop）
     * @param jrpcCB  JRPC 请求到达时的外部回调
     * @return true 初始化成功
     */
    bool Open(TapDelegateJrpcRequestReadCB jrpcCB);

    /**
     * @brief 关闭路由层，逆序释放所有 TAP 组件和事件循环
     *
     * 释放顺序严格不可变：TAP 上下文先清（Drop 回调时 delegate 仍存活）
     * → HubProxy delegate 停止 → JRPC delegate 停止 → ZmEvBaseRunLoop 停止
     *
     * @param beforeLoopStop 可选，在 delegate 停止后、事件循环停止前执行的回调
     *                       用于清理依赖 event_base 的资源（如 bufferevent_pair 池）
     */
    void Close(std::function<void()> beforeLoopStop = nullptr);

    // --- 供协议前端访问的只读接口 ---

    /** @brief 获取 libevent 事件循环基 */
    event_base*      EvBase()     const { return m_evLoop ? m_evLoop->GetEventBase() : nullptr; }
    /** @brief 获取 libevent DNS 解析基 */
    evdns_base*      EvDnsBase()  const { return m_evLoop ? m_evLoop->GetEventDnsBase() : nullptr; }
    /** @brief 获取 TAP 上下文池 */
    ZmTapContext*    TapContext() const { return m_tapContext; }
    /** @brief 获取 Hub 代理 */
    ZmTapHubProxy*   HubProxy()   const { return m_tapHubProxy; }

    /** @brief 查询 Hub 路由层是否已启动 */
    bool IsOpen() const { return m_tapHubProxy != nullptr; }
    /** @brief 查询 JRPC Delegate 是否已启动 */
    bool IsJrpcProxyOpen() const { return m_tapDelegateJRPC != nullptr; }
    /** @brief 查询事件循环是否就绪（可用于跨线程调度前的检查） */
    bool IsLooped() const { return m_evLoop && m_evLoop->IsLooped(); }

private:
    ZmTapContext*      m_tapContext;       ///< TAP 上下文池（所有前端共享）
    ZmTapDelegateJRPC* m_tapDelegateJRPC;  ///< JRPC 协议委托处理器
    ZmTapHubProxy*     m_tapHubProxy;      ///< Hub 代理（共享消息路由）
    uint16_t           m_hubSocks5Port;    ///< Hub 代理监听端口，用于转发 socks5 代理
    ZmEvBaseRunLoop*   m_evLoop;           ///< libevent 事件循环线程（Hub 拥有所有权）
};

#endif // HUB_PROXY_MANAGER_H
