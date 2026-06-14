#include "hub_proxy_manager.h"
#include "zm_logger.h"
#include "service_define.h"

HubProxyManager::HubProxyManager()
    : m_tapContext(nullptr)
    , m_tapDelegateJRPC(nullptr)
    , m_tapHubProxy(nullptr)
    , m_evLoop(nullptr)
{
}

HubProxyManager::~HubProxyManager()
{
    Close();
}

bool HubProxyManager::Open(TapDelegateJrpcRequestReadCB jrpcCB)
{
    // 1. 创建并启动事件循环线程
    if (!m_evLoop)
    {
        m_evLoop = new ZmEvBaseRunLoop("HubLoop");
        if (!m_evLoop->Loop())
        {
            DEFAULT_LOG_ERROR("HubProxyManager::Open failed: ZmEvBaseRunLoop::Loop() returned false");
            delete m_evLoop;
            m_evLoop = nullptr;
            return false;
        }
    }

    event_base* evbase = m_evLoop->GetEventBase();
    evdns_base* evdnsbase = m_evLoop->GetEventDnsBase();

    // 2. 创建 JRPC 协议委托处理器
    if (nullptr == m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC = new ZmTapDelegateJRPC();
        m_tapDelegateJRPC->StartTapDelegate(evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC);
        m_tapDelegateJRPC->SetJrpcRequestReadCB(jrpcCB);
    }

    // 3. 创建 TAP 上下文池和 Hub 代理（共享路由层）
    if (nullptr == m_tapHubProxy)
    {
        if (nullptr == m_tapContext)
            m_tapContext = new ZmTapContext();

        m_tapHubProxy = new ZmTapHubProxy();
        m_tapHubProxy->SetJrpcDelegate(m_tapDelegateJRPC);
        m_tapHubProxy->SetEvDns(evdnsbase);
        m_tapHubProxy->StartTapDelegate(m_tapContext, evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
        m_hubSocks5Port = m_tapHubProxy->AddListenPort(ZM_SOCKS5_SERVER_PORT);
    }

    return (nullptr != m_tapHubProxy);
}

void HubProxyManager::Close()
{
    // ★ 释放顺序严格不可变：
    //   ① TAP 上下文池先清理 — Drop 每个 TAP（释放 bufferevent），此时 delegate 仍存活
    //   ② HubProxy delegate 再停止 — 关闭 evconnlisteners，释放 m_evdelegate
    //   ③ JRPC delegate 最后停止 — 释放 m_evdelegate
    //   ④ ZmEvBaseRunLoop 最后停止 — 确保以上所有 libevent 资源释放完毕
    //   逆序原因：TAP 的 delegate 指向 HubProxy 或 JRPC，Drop 回调需 delegate 存活
    if (m_tapContext)
    {
        m_tapContext->Clear();
        delete m_tapContext;
        m_tapContext = nullptr;
    }

    if (m_tapHubProxy)
    {
        m_tapHubProxy->StopTapDelegate();
        delete m_tapHubProxy;
        m_tapHubProxy = nullptr;
    }

    if (m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC->StopTapDelegate();
        delete m_tapDelegateJRPC;
        m_tapDelegateJRPC = nullptr;
    }

    m_hubSocks5Port = 0;

    // 最后停止事件循环线程
    if (m_evLoop)
    {
        m_evLoop->Stop();
        delete m_evLoop;
        m_evLoop = nullptr;
    }
}
