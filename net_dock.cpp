#include "net_dock.h"

#include "zm_logger.h"

NetDock::NetDock()
    : m_dockRunloop(nullptr)
    , m_evbase(nullptr)
    , m_messageServerMgr(nullptr)
    , m_hubProxyMgr(nullptr)
    , m_httpJsonRpcMgr(nullptr)
    , m_unInited(false)
{
}

NetDock::~NetDock()
{
    UnInit();
}

void NetDock::UnInit()
{
    if (m_unInited)
        return;
    m_unInited = true;

    // ★ 关闭顺序严格不可变：
    //   ① 前端服务器先停 — 不再产生新的请求（Worker 线程终止，不再有 ScheduleTaskInLoop 调用）
    //   ② Hub 路由层再停 — 释放 TAP 组件（其 event/bufferevent 均注册在 _evbase 上）
    //   ③ 取消残留的调度任务 — 释放未触发的 ctx（正常情况此集合已空，兜底用）
    //   ④ DockRunLoop 最后停 — 退出事件循环，freeEventObjects 安全清理 _evbase
    CloseHttpJsonRpcServer();
    CloseSocks5Server();
    CloseWebSocketServer();

    CloseHub();

    // 前端已停，不会再有新的 ScheduleTaskInLoop 调用。清理可能残留的 ctx
    {
        std::lock_guard<std::mutex> lock(m_scheduleMutex);
        for (auto* ctx : m_pendingScheduleCtx)
        {
            if (ctx->ev_schedule)
            {
                event_free(ctx->ev_schedule);
            }
            delete ctx;
        }
        m_pendingScheduleCtx.clear();
    }

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

bool NetDock::ScheduleTaskInLoop(std::function<void(void*)> fn_run, std::function<void(void*)> fn_cb, void* param)
{
    if (m_dockRunloop->IsLooped())
    {
        ZM_SCHEDULE_CTX* ctx = new ZM_SCHEDULE_CTX();
        ctx->ev_schedule = nullptr;
        ctx->fn_run = fn_run;
        ctx->fn_cb = fn_cb;
        ctx->param = param;
        ctx->owner = this;

        // fd=-1 表示纯手动触发事件，events=0 不监听任何 I/O
        ctx->ev_schedule = event_new(m_evbase,
            -1, 0, NetDock::OnScheduleEventCB, ctx);
        event_add(ctx->ev_schedule, NULL);

        // 记录到 pending 集合，供 UnInit 在关闭前取消未触发的任务
        {
            std::lock_guard<std::mutex> lock(m_scheduleMutex);
            m_pendingScheduleCtx.insert(ctx);
        }

        // 立即激活一次性调度事件
        event_active(ctx->ev_schedule, EV_TIMEOUT, 0);
        return true;
    }
    return false;
}

void NetDock::OnScheduleEventCB(evutil_socket_t fd, short what, void* pctx)
{
    if (!pctx)
        return;

    ZM_SCHEDULE_CTX* scheduleCtx = (ZM_SCHEDULE_CTX*)pctx;
    NetDock* self = (NetDock*)scheduleCtx->owner;

    // 从 pending 集合移除
    if (self)
    {
        std::lock_guard<std::mutex> lock(self->m_scheduleMutex);
        self->m_pendingScheduleCtx.erase(scheduleCtx);
    }

    // 执行任务
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
