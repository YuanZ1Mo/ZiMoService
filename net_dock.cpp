#include "net_dock.h"

#include "http_jsonrpc_manager.h"
#include "http_restful_manager.h"
#include "http_server_manager.h"
#include "broadcast_manager.h"

#include "zm_net_socket.h"
#include "zm_ssl_ctx.h"
#include "zm_util_sys.h"
#include "zm_util_logger.h"
#include "zm_util_json.h"

NetDock::NetDock()
    : m_httpJsonRpcMgr(nullptr)
    , m_httpRestfulMgr(nullptr)
    , m_httpServerMgr(nullptr)
    , m_broadcastMgr(nullptr)
    , m_ticketRotator(nullptr)
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

    // ① 先停 ticket 轮换器:关闭过程中不再投递密钥,避免访问正在关闭的 manager
    if (m_ticketRotator)
    {
        m_ticketRotator->Stop();
        delete m_ticketRotator;
        m_ticketRotator = nullptr;
    }

    // ① 独立组件先关
    CloseBroadcastServer();
    CloseHttpServer();

    // ② HTTP 前端软关闭（停 HTTP Server + 排空 worker + 停 ZmReqLoopPool）
    CloseHttpJsonRpcServer();
    CloseHttpRESTfulServer();

    // ③ 最后 delete（析构兜底，幂等）
    if (m_httpJsonRpcMgr)
    {
        delete m_httpJsonRpcMgr;
        m_httpJsonRpcMgr = nullptr;
    }

    if (m_httpRestfulMgr)
    {
        delete m_httpRestfulMgr;
        m_httpRestfulMgr = nullptr;
    }
}

void NetDock::OpenHttpJsonRpcServer()
{
    if (!m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr = new HttpJsonRpcManager();
        // 业务回调须在 Open() 前注入（Open 内部经 ZmReqLoopPool投递后才会被调用）
        m_httpJsonRpcMgr->SetJrpcRequestReadCB(m_jrpcRequestReadCB);
        if (!m_httpJsonRpcMgr->Open())
        {
            DEFAULT_LOG_ERROR("OpenHttpJsonRpcServer failed: HttpJsonRpcManager::Open() returned false");
            delete m_httpJsonRpcMgr;
            m_httpJsonRpcMgr = nullptr;
        }
        else
        {
            // HTTPS 模式:注入共享 ticket 密钥,启动/轮换统一经事件循环线程投递
            // (HTTP 模式零开销,不创建 rotator;避免主线程与 loop 线程并发写 SSL_CTX)
            if (m_httpJsonRpcMgr->IsOpen() && m_httpJsonRpcMgr->IsHttps())
            {
                ZmTicketKeyRotator* rotator = EnsureTicketRotator();
                if (rotator)
                    m_httpJsonRpcMgr->PostSetTicketKeys(
                        rotator->GetTicketManager().Key(), ZM_TICKET_KEYS_LEN);
            }
        }
    }
}

void NetDock::CloseHttpJsonRpcServer()
{
    // ★ 仅执行软关闭（停 HTTP Server + 排空 worker + 停 ZmReqLoopPool），不 delete 对象
    // delete 推迟到 UnInit() 统一执行
    if (m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr->Close();
    }
}

void NetDock::OpenHttpRESTfulServer()
{
    if (!m_httpRestfulMgr)
    {
        m_httpRestfulMgr = new HttpRestfulManager();
        // 业务回调须在 Open() 前注入（Open 内部经 ZmReqLoopPool投递后才会被调用）
        m_httpRestfulMgr->SetRESTfulRequestCB(m_restfulRequestCB);
        // WebSocket 业务回调(HTTP 与 RESTful 服务器共用,同上 Open 前注入)
        m_httpRestfulMgr->SetWebSocketCallbacks(m_websocketCallbacks);
        if (!m_httpRestfulMgr->Open())
        {
            DEFAULT_LOG_ERROR("OpenHttpRESTfulServer failed: HttpRestfulManager::Open() returned false");
            delete m_httpRestfulMgr;
            m_httpRestfulMgr = nullptr;
        }
        else
        {
            // HTTPS 模式:注入共享 ticket 密钥,启动/轮换统一经事件循环线程投递
            // (HTTP 模式零开销,不创建 rotator;避免主线程与 loop 线程并发写 SSL_CTX)
            if (m_httpRestfulMgr->IsOpen() && m_httpRestfulMgr->IsHttps())
            {
                ZmTicketKeyRotator* rotator = EnsureTicketRotator();
                if (rotator)
                    m_httpRestfulMgr->PostSetTicketKeys(
                        rotator->GetTicketManager().Key(), ZM_TICKET_KEYS_LEN);
            }
        }
    }
}

void NetDock::CloseHttpRESTfulServer()
{
    // ★ 仅执行软关闭（停 HTTP Server + 排空 worker + 停 ZmReqLoopPool），不 delete 对象
    // delete 推迟到 UnInit() 统一执行
    if (m_httpRestfulMgr)
    {
        m_httpRestfulMgr->Close();
    }
}

bool NetDock::IsRESTfulHttpOpen() const
{
    return m_httpRestfulMgr && m_httpRestfulMgr->IsOpen();
}

void NetDock::OpenHttpServer()
{
    if (!m_httpServerMgr)
    {
        m_httpServerMgr = new HttpServerManager();
        m_httpServerMgr->Open();
        // HTTPS 模式:注入共享 ticket 密钥,启动/轮换统一经事件循环线程投递
        // (HTTP 模式零开销,不创建 rotator;避免主线程与 loop 线程并发写 SSL_CTX)
        if (m_httpServerMgr->IsOpen() && m_httpServerMgr->IsHttps())
        {
            ZmTicketKeyRotator* rotator = EnsureTicketRotator();
            if (rotator)
                m_httpServerMgr->PostSetTicketKeys(
                    rotator->GetTicketManager().Key(), ZM_TICKET_KEYS_LEN);
        }
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

// ============================================================================
// TLS session ticket 轮换器(方案 B:懒创建,所有 HTTPS 服务器共享一把密钥)
// ============================================================================

ZmTicketKeyRotator* NetDock::EnsureTicketRotator()
{
    if (m_ticketRotator)
        return m_ticketRotator;

    // 从 exe 路径推导证书文件(certs/ 与 exe 同目录,与 HttpServerManager::Open 一致)
    char exePath[MAX_PATH];
    ZmSystem::GetModuleDir(exePath, MAX_PATH);
    std::string ticketFile = std::string(exePath) + "\\certs\\ticket.key";

    m_ticketRotator = new ZmTicketKeyRotator();
    if (!m_ticketRotator->Init(ticketFile.c_str()))
    {
        // 初始化失败:回退 OpenSSL 内部随机密钥。
        // 不返回 rotator,避免把全零密钥安装到各服务器
        DEFAULT_LOG_ERROR("TicketRotator 初始化失败,回退 OpenSSL 内部随机密钥(恢复不跨重启)");
        delete m_ticketRotator;
        m_ticketRotator = nullptr;
        return nullptr;
    }
    if (!m_ticketRotator->Start(12 * 3600, std::bind(&NetDock::OnTicketRotated, this)))
    {
        DEFAULT_LOG_ERROR("TicketRotator 启动失败,禁用定时轮换");
    }
    else
    {
        DEFAULT_LOG_INFO("TicketRotator 已启用:密钥文件 {},轮换间隔 12h", ticketFile);
    }

    return m_ticketRotator;
}

void NetDock::OnTicketRotated()
{
    // 在 rotator 线程执行;经 event_base_once 投递到各服务器事件循环线程
    const unsigned char* key = m_ticketRotator->GetTicketManager().Key();
    if (m_httpServerMgr)    m_httpServerMgr->PostSetTicketKeys(key, ZM_TICKET_KEYS_LEN);
    if (m_httpJsonRpcMgr)   m_httpJsonRpcMgr->PostSetTicketKeys(key, ZM_TICKET_KEYS_LEN);
    if (m_httpRestfulMgr)   m_httpRestfulMgr->PostSetTicketKeys(key, ZM_TICKET_KEYS_LEN);

    DEFAULT_LOG_INFO("TLS session ticket 密钥已轮换,已投递到各 HTTPS 服务器");
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

void NetDock::SetJrpcRequestReadCB(ZmReqLoopJrpcRequestCB cb)
{
    m_jrpcRequestReadCB = cb;
}

void NetDock::SetRESTfulRequestCB(ZmReqLoopRestfulRequestCB cb)
{
    m_restfulRequestCB = cb;
}

void NetDock::SetWebSocketCallbacks(ZmWebSocketCallbacks cb)
{
    m_websocketCallbacks = std::move(cb);
}

void NetDock::OpenBroadcastServer()
{
    if (!m_broadcastMgr)
    {
        m_broadcastMgr = new BroadcastManager();
        m_broadcastMgr->Open();
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

