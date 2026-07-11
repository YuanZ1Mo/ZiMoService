#include "hub_proxy_manager.h"

#include "service_define.h"

#include "zm_net_tap_hub.h"
#include "zm_logger.h"

#include <future>

HubProxyManager::HubProxyManager()
    : m_tapContext(nullptr)
    , m_tapDelegateJRPC(nullptr)
    , m_tapDelegateRESTful(nullptr)
    , m_tapHubProxy(nullptr)
    , m_evLoopHub(nullptr)
    , m_evLoopJRPC(nullptr)
    , m_evLoopRESTful(nullptr)
{
}

HubProxyManager::~HubProxyManager()
{
    Close();
}

bool HubProxyManager::Open(TapDelegateJrpcRequestReadCB jrpcCB, TapDelegateRESTfulRequestCB restCB)
{

    if (nullptr == m_tapContext)
    {
        m_tapContext = new ZmTapContext();
    }

    // 1. 创建并启动事件循环线程
    if (!m_evLoopHub)
    {
        m_evLoopHub = new ZmEvBaseRunLoop("HubProxyLoop");
        if (!m_evLoopHub->Loop())
        {
            DEFAULT_LOG_ERROR("HubProxyManager::Open failed: ZmEvBaseRunLoop::Loop() returned false");
            delete m_evLoopHub;
            m_evLoopHub = nullptr;
            return false;
        }
    }

    if (!m_evLoopJRPC)
    {
        m_evLoopJRPC = new ZmEvBaseRunLoop("DelegateJRPCLoop");
        if (!m_evLoopJRPC->Loop())
        {
            DEFAULT_LOG_ERROR("HubProxyManager::Open failed: ZmEvBaseRunLoop::Loop() returned false");
            delete m_evLoopJRPC;
            m_evLoopJRPC = nullptr;
            return false;
        }
    }


    // 2. 创建 JRPC 协议委托处理器
    if (nullptr == m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC = new ZmTapDelegateJRPC(m_evLoopJRPC->GetEventBase());
        m_tapDelegateJRPC->StartTapDelegate(ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC);
        m_tapDelegateJRPC->SetEvDns(m_evLoopJRPC->GetEventDnsBase());
        m_tapDelegateJRPC->SetJrpcRequestReadCB(jrpcCB);
    }

    // 2.5 创建 RESTful 协议委托处理器
    if (nullptr == m_tapDelegateRESTful)
    {
        if (!m_evLoopRESTful)
        {
            m_evLoopRESTful = new ZmEvBaseRunLoop("DelegateRESTfulLoop");
            if (!m_evLoopRESTful->Loop())
            {
                DEFAULT_LOG_ERROR("HubProxyManager::Open failed: RESTful event loop start failed");
                delete m_evLoopRESTful;
                m_evLoopRESTful = nullptr;
                return false;
            }
        }

        m_tapDelegateRESTful = new ZmTapDelegateRESTful(m_evLoopRESTful->GetEventBase());
        m_tapDelegateRESTful->StartTapDelegate(ZM_DELEGATE_MODE_PROXY_INTERNAL_RESTFUL);
        m_tapDelegateRESTful->SetEvDns(m_evLoopRESTful->GetEventDnsBase());
        if (restCB)
            m_tapDelegateRESTful->SetRESTfulRequestCB(restCB);
    }

    // 3. 创建 TAP 上下文池和 Hub 代理（共享路由层）
    if (nullptr == m_tapHubProxy)
    {
        m_tapHubProxy = new ZmTapHubProxy(m_evLoopHub->GetEventBase());
        m_tapHubProxy->SetJrpcDelegate(m_tapDelegateJRPC);
        if (m_tapDelegateRESTful)
            m_tapHubProxy->SetRESTfulDelegate(m_tapDelegateRESTful);
        m_tapHubProxy->SetEvDns(m_evLoopHub->GetEventDnsBase());
        m_tapHubProxy->StartTapDelegate(ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
        //m_hubSocks5Port = m_tapHubProxy->AddListenPort(ZM_SOCKS5_SERVER_PORT);
        ZmTapContextEventHandler::RegistryContextEventHandler("HubProxy", m_tapContext, m_tapHubProxy);
    }

    return true;
}

void HubProxyManager::Close(std::function<void()> beforeLoopStop)
{
    // ★ 释放顺序严格不可变：
    //   ⓪ JRPC delegate 先停线程池 — join 所有 worker，确保无人持有 TAP 指针
    //   ① TAP 上下文池再清理 — Drop 每个 TAP（释放 bufferevent），此时 delegate 仍存活
    //   ② HubProxy delegate 停止 — 关闭 evconnlisteners，释放 m_evdelegate
    //   ③ JRPC delegate 最后停止 — 释放 m_evdelegate
    //   ④ beforeLoopStop 回调 — 在事件循环停止前清理依赖 event_base 的资源（如 pair 池）
    //   ⑤ ZmEvBaseRunLoop 最后停止 — 确保以上所有 libevent 资源释放完毕
    //   逆序原因：TAP 的 delegate 指向 HubProxy 或 JRPC，Drop 回调需 delegate 存活

    ZmTapContextEventHandler::UnregistryContextEventHandler("HubProxy");

    if (m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC->StopThreadPool();
    }

    if (m_tapDelegateRESTful)
    {
        m_tapDelegateRESTful->StopThreadPool();
    }

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

    if (m_tapDelegateRESTful)
    {
        m_tapDelegateRESTful->StopTapDelegate();
        delete m_tapDelegateRESTful;
        m_tapDelegateRESTful = nullptr;
    }

    m_hubSocks5Port = 0;

    // ★ 在事件循环停止前执行外部回调（如 pair 池 shutdown，其 bufferevent_free 依赖 event_base 存活）
    if (beforeLoopStop)
    {
        beforeLoopStop();
    }

    // 最后停止事件循环线程
    if (m_evLoopHub)
    {
        m_evLoopHub->Stop();
        delete m_evLoopHub;
        m_evLoopHub = nullptr;
    }

    if (m_evLoopJRPC)
    {
        m_evLoopJRPC->Stop();
        delete m_evLoopJRPC;
        m_evLoopJRPC = nullptr;
    }

    if (m_evLoopRESTful)
    {
        m_evLoopRESTful->Stop();
        delete m_evLoopRESTful;
        m_evLoopRESTful = nullptr;
    }
}
