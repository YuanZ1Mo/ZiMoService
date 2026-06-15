#include "net_dock.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_net_tap.h"
#include "zm_net_socket.h"

NetDock::NetDock()
    : m_hubProxyMgr(nullptr)
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
    //   ① HTTP 前端先软关闭 — Close 内部先关 ZmNetRequestChannel（拒绝 pending promise）再 join 线程池
    //      Pair 池暂不销毁：已注入 Hub 链的在飞请求仍需 pair 完成响应回写
    //   ② Hub 路由层停止 — 内部 Drop 所有 TAP（在飞请求的 pair[1] 被标记归还）
    //      → 销毁 pair 池（在事件循环停止前，bufferevent_free 依赖 event_base 存活）
    //      → 停止 ZmEvBaseRunLoop
    //      Hub 停止后无新回调触发，pair 已全部释放
    //   ③ HttpJsonRpcManager delete — 析构（pair 池已在步骤②中销毁，此处为 nullptr 不重复操作）
    CloseHttpServer();
    CloseHttpJsonRpcServer();  // 软关闭（不 delete）
    CloseHub();
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
        // ★ 在 Hub 事件循环停止前销毁 pair 池（bufferevent_free 依赖 event_base 存活）
        m_hubProxyMgr->Close([this]() {
            if (m_httpJsonRpcMgr)
            {
                m_httpJsonRpcMgr->ShutdownPairPool();
            }
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
        if (!m_httpJsonRpcMgr->Open(m_hubProxyMgr))
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
    return m_hubProxyMgr && m_hubProxyMgr->IsOpen();
}

bool NetDock::IsJrpcProxyOpen() const
{
    return m_hubProxyMgr && m_hubProxyMgr->IsJrpcProxyOpen();
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

