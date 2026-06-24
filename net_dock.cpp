#include "net_dock.h"

#include "hub_proxy_manager.h"
#include "http_jsonrpc_manager.h"
#include "http_server_manager.h"
#include "broadcast_manager.h"

#include "zm_logger.h"
#include "zm_json.h"
#include "zm_net_tap.h"
#include "zm_net_socket.h"

NetDock::NetDock()
    : m_hubProxyMgr(nullptr)
    , m_httpJsonRpcMgr(nullptr)
    , m_httpServerMgr(nullptr)
    , m_broadcastMgr(nullptr)
    , m_unInited(false)
{
}

NetDock::~NetDock()
{
    UnInit();
}

void NetDock::Init()
{
    ZmWinSockHelper::Init();
}

void NetDock::UnInit()
{
    if (m_unInited)
        return;
    m_unInited = true;

    // ① 独立组件先关
    CloseBroadcastServer();
    CloseHttpServer();

    // ② HTTP JRPC 软关闭（停 HTTP Server + 线程池，pair 池保留给在飞请求）
    CloseHttpJsonRpcServer();

    // ③ Hub 关闭：StopThreadPool → 清所有 TAP（pair1 全部 EOF → pair 归还池）
    //    → beforeLoopStop 回调销毁 pair 池 → 停 event loop
    CloseHub();

    // ④ 最后 delete（pair 池已在步骤③中销毁，析构中 ShutdownPairPool 是 nullptr 跳过）
    if (m_httpJsonRpcMgr)
    {
        delete m_httpJsonRpcMgr;
        m_httpJsonRpcMgr = nullptr;
    }
}

void NetDock::OpenHub()
{
    if (!m_hubProxyMgr)
    {
        m_hubProxyMgr = new HubProxyManager();
        m_hubProxyMgr->Open(m_jrpcRequestReadCB);
    }
}

void NetDock::CloseHub()
{
    if (m_hubProxyMgr)
    {
        // ★ 在 Hub 清完所有 TAP 后、事件循环停止前，销毁 pair 池
        m_hubProxyMgr->Close([this]() {
            if (m_httpJsonRpcMgr)
                m_httpJsonRpcMgr->ShutdownPairPool();
        });
        delete m_hubProxyMgr;
        m_hubProxyMgr = nullptr;
    }
}

void NetDock::OpenHttpJsonRpcServer()
{
    if (!m_hubProxyMgr)
    {
        DEFAULT_LOG_ERROR("OpenHttpJsonRpcServer failed: Hub not started, call OpenHub() first");
    }

    if (!m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr = new HttpJsonRpcManager();
        // 从 Hub 获取 event_base，HttpJsonRpcManager 内部自行创建 ZmNetRequestChannel 并绑定 Hub 注入 handler
        if (!m_httpJsonRpcMgr->Open())
        {
            DEFAULT_LOG_ERROR("OpenHttpJsonRpcServer failed: HttpJsonRpcManager::Open() returned false");
            delete m_httpJsonRpcMgr;
            m_httpJsonRpcMgr = nullptr;
        }
    }
}

void NetDock::CloseHttpJsonRpcServer()
{
    // ★ 仅执行软关闭（停止通道 + HTTP 服务器），不 delete 对象
    // Pair 池需在 Hub 关闭（所有 TAP 已 Drop）后才安全销毁，
    // 因此 delete 推迟到 UnInit() 中 CloseHub() 之后执行
    if (m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr->Close();
    }
}

void NetDock::OpenHttpServer(const char* wwwRoot)
{
    if (!m_httpServerMgr)
    {
        m_httpServerMgr = new HttpServerManager();
        m_httpServerMgr->Open(wwwRoot);
    }
}

void NetDock::CloseHttpServer()
{
    if (m_httpServerMgr)
    {
        m_httpServerMgr->Close();
        delete m_httpServerMgr;
        m_httpServerMgr = nullptr;
    }
}

ZmHttpRouter& NetDock::GetHttpRouter()
{
    return m_httpServerMgr->GetRouter();
}

HttpServerManager* NetDock::GetHttpServerManager()
{
    return m_httpServerMgr;
}

bool NetDock::IsHttpOpen() const
{
    return m_httpServerMgr && m_httpServerMgr->IsOpen();
}

bool NetDock::IsJrpcHttpOpen() const
{
    return m_httpJsonRpcMgr && m_httpJsonRpcMgr->IsOpen();
}

bool NetDock::IsHubOpen() const
{
    return m_hubProxyMgr && m_hubProxyMgr->IsHubOpen();
}

bool NetDock::IsJrpcProxyOpen() const
{
    return m_hubProxyMgr && m_hubProxyMgr->IsJrpcDelegateOpen();
}

void NetDock::OpenSocks5Server()
{
    if (!m_hubProxyMgr)
    {
        DEFAULT_LOG_ERROR("OpenSocks5Server failed: Hub not started, call OpenHub() first");
    }
    // TODO: 后续实现 SOCKS5 前端
}

void NetDock::CloseSocks5Server()
{
    // TODO: 后续实现
}

void NetDock::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
    m_jrpcRequestReadCB = cb;
}

void NetDock::OpenBroadcastServer(uint16_t port)
{
    if (!m_broadcastMgr)
    {
        m_broadcastMgr = new BroadcastManager();
        m_broadcastMgr->Open(port);
    }
}

void NetDock::CloseBroadcastServer()
{
    if (m_broadcastMgr)
    {
        m_broadcastMgr->Close();
        delete m_broadcastMgr;
        m_broadcastMgr = nullptr;
    }
}

BroadcastManager* NetDock::GetBroadcastManager()
{
    return m_broadcastMgr;
}

bool NetDock::IsBroadcastOpen() const
{
    return m_broadcastMgr && m_broadcastMgr->IsOpen();
}

