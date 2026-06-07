#include "net_dock.h"

#include "zm_logger.h"

NetDock::NetDock()
    : m_dockRunloop(nullptr)
    , m_evbase(nullptr)
    , m_messageServerMgr(nullptr)
    , m_hubProxyMgr(nullptr)
    , m_httpJsonRpcMgr(nullptr)
{
}

NetDock::~NetDock()
{
    CloseWebSocketServer();
    CloseHttpJsonRpcServer();
    CloseSocks5Server();
    CloseHub();

    if (m_dockRunloop)
    {
        m_dockRunloop->Stop();
        delete m_dockRunloop;
        m_dockRunloop = nullptr;
    }
}

void NetDock::Init()
{
    if (!m_dockRunloop)
    {
        m_dockRunloop = new DockRunLoop();
        if (m_dockRunloop->Loop())
        {
            m_evbase = m_dockRunloop->GetEventBase();
        }
    }
}

void NetDock::OpenWebSocketServer()
{
    if (!m_messageServerMgr)
    {
        m_messageServerMgr = new MessageServerManager();
    }
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
        m_hubProxyMgr->Open(
            m_evbase,
            m_dockRunloop->GetEventDnsBase(),
            m_jrpcRequestReadCB);
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
        DEFAULT_LOG_ERROR("OpenHttpJsonRpcServer warning: Hub not started, call OpenHub() first");
    }

    if (!m_httpJsonRpcMgr)
    {
        m_httpJsonRpcMgr = new HttpJsonRpcManager();
        // 注入事件循环调度能力
        m_httpJsonRpcMgr->SetScheduleFn(
            std::bind(&NetDock::ScheduleTaskInLoop, this,
                std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        m_httpJsonRpcMgr->Open(m_hubProxyMgr);
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

void NetDock::OpenSocks5Server()
{
    if (!m_hubProxyMgr)
    {
        DEFAULT_LOG_ERROR("OpenSocks5Server warning: Hub not started, call OpenHub() first");

    }
    // TODO: 后续实现 SOCKS5
}

void NetDock::CloseSocks5Server()
{
    // TODO: 后续实现
}

void NetDock::SetJrpcRequestReadCB(TapDelegateJrpcRequestReadCB cb)
{
    m_jrpcRequestReadCB = cb;
}

typedef struct struct_schedule_ctx
{
    event* ev_schedule;
    std::function<void(void*)>  fn_run;
    std::function<void(void*)>  fn_cb;
    void* param;

    // 默认构造函数
    struct_schedule_ctx() {
        ev_schedule = nullptr;
        param = nullptr;
    }

}ZM_SCHEDULE_CTX;

bool NetDock::ScheduleTaskInLoop(std::function<void(void*)> fn_run, std::function<void(void*)> fn_cb, void* param)
{
    if (m_dockRunloop->IsLooped())
    {
        ZM_SCHEDULE_CTX* ctx = new ZM_SCHEDULE_CTX();
        ctx->fn_run = fn_run;
        ctx->fn_cb = fn_cb;
        ctx->param = param;
        // fd=-1 表示纯手动触发事件，events=0 不监听任何 I/O
        ctx->ev_schedule = event_new(m_evbase,
            -1, 0, NetDock::OnScheduleEventCB, ctx);
        event_add(ctx->ev_schedule, NULL);
        // 立即激活一次性调度事件，EV_TIMEOUT 表示用后即弃
        event_active(ctx->ev_schedule, EV_TIMEOUT, 0);
        /** TODO: 需要将ctx存储起来，以便事件还未触发时可取消和删除 */
        return true;
    }
    return false;
}

void NetDock::OnScheduleEventCB(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        ZM_SCHEDULE_CTX* scheduleCtx = (ZM_SCHEDULE_CTX*)ctx;
        if (scheduleCtx->ev_schedule)
        {
            event_free(scheduleCtx->ev_schedule);
        }
        if (scheduleCtx->fn_run)
        {
            scheduleCtx->fn_run(scheduleCtx->param);
        }
        if (scheduleCtx->fn_cb)
        {
            scheduleCtx->fn_cb(scheduleCtx->param);
        }
        delete scheduleCtx;
    }
}
