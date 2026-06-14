#include "net_dock.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_net_tap.h"
#include "zm_net_socket.h"

NetDock::NetDock()
    : m_messageServerMgr(nullptr)
    , m_hubProxyMgr(nullptr)
    , m_httpJsonRpcMgr(nullptr)
    , m_httpServerMgr(nullptr)
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

    // ★ 关闭顺序严格不可变：
    //   ① HTTP 前端先停 — Close 内部先关 ZmNetRequestChannel（拒绝 pending promise，唤醒 Worker）再 join 线程池
    //   ② Hub 路由层停止 — 内部释放 delegate（含 pending 调度清理）→ 停止 ZmEvBaseRunLoop
    CloseHttpServer();
    CloseSocks5Server();
    CloseWebSocketServer();
    CloseHttpJsonRpcServer();
    CloseHub();
}

void NetDock::OpenWebSocketServer()
{
    if (!m_messageServerMgr)
        m_messageServerMgr = new MessageServerManager();
    m_messageServerMgr->Open();
}

void NetDock::CloseWebSocketServer()
{
    if (m_messageServerMgr)
    {
        m_messageServerMgr->Close();
        delete m_messageServerMgr;
        m_messageServerMgr = nullptr;
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
        m_hubProxyMgr->Close();
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
        if (!m_httpJsonRpcMgr->Open(m_hubProxyMgr->EvBase(), m_hubProxyMgr))
        {
            DEFAULT_LOG_ERROR("OpenHttpJsonRpcServer failed: HttpJsonRpcManager::Open() returned false");
            delete m_httpJsonRpcMgr;
            m_httpJsonRpcMgr = nullptr;
        }
    }
}

void NetDock::CloseHttpJsonRpcServer()
{
    if (m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr->Close();
        delete m_httpJsonRpcMgr;
        m_httpJsonRpcMgr = nullptr;
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
    return m_hubProxyMgr && m_hubProxyMgr->IsOpen();
}

bool NetDock::IsJrpcProxyOpen() const
{
    return m_hubProxyMgr && m_hubProxyMgr->IsJrpcProxyOpen();
}

bool NetDock::IsWebSocketOpen() const
{
    return m_messageServerMgr && m_messageServerMgr->IsOpen();
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

