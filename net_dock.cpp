#include "net_dock.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_net_tap.h"

NetDock::NetDock()
    : m_dockRunloop(nullptr)
    , m_evbase(nullptr)
    , m_messageServerMgr(nullptr)
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
    if (!m_dockRunloop)
    {
        m_dockRunloop = new DockRunLoop();
        if (m_dockRunloop->Loop())
            m_evbase = m_dockRunloop->GetEventBase();
    }
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
    CloseHttpServer();
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
                event_free(ctx->ev_schedule);
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

    // evbase 由 DockRunLoop 内部释放，NetDock 持有的裸指针置空防止误用
    m_evbase = nullptr;
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
        m_hubProxyMgr->Open(m_evbase, m_dockRunloop->GetEventDnsBase(), m_jrpcRequestReadCB);
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
        // 注入事件循环调度能力，使 HttpJsonRpcManager 可跨线程交付 bufferevent pair
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

bool NetDock::ScheduleTaskInLoop(std::function<void(void*)> fn_run, std::function<void(void*)> fn_cb, void* param)
{
    if (m_evbase && m_dockRunloop && m_dockRunloop->IsLooped())
    {
        ZM_SCHEDULE_CTX* ctx = new ZM_SCHEDULE_CTX();
        ctx->ev_schedule = nullptr;
        ctx->fn_run = fn_run;
        ctx->fn_cb = fn_cb;
        ctx->param = param;
        ctx->owner = this;

        // fd=-1 表示纯手动触发事件，events=0 不监听任何 I/O
        ctx->ev_schedule = event_new(m_evbase, -1, 0, NetDock::OnScheduleEventCB, ctx);
        event_add(ctx->ev_schedule, NULL);

        // 记录到 pending 集合，供 UnInit 在关闭前取消未触发的任务
        {
            std::lock_guard<std::mutex> lock(m_scheduleMutex);
            m_pendingScheduleCtx.insert(ctx);
        }

        // 立即激活一次性调度事件，EV_TIMEOUT 表示用后即弃
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

    // 执行 fn_run → fn_cb 后释放 ctx
    if (scheduleCtx->ev_schedule)
        event_free(scheduleCtx->ev_schedule);
    if (scheduleCtx->fn_run)
        scheduleCtx->fn_run(scheduleCtx->param);
    if (scheduleCtx->fn_cb)
        scheduleCtx->fn_cb(scheduleCtx->param);
    delete scheduleCtx;
}

// ============================================================================
// TAP 通用响应操作（必须在 libevent 线程中调用）
// ============================================================================

void NetDock::Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse)
{
    ZmTapDelegate* back_delegate = ZmTapContext::BackChainPop(tap);
    if (back_delegate)
    {
        std::string jstr = jsResponse.dump();
        ZmTapContext::SetOnBackData(tap, jstr.size(), jstr.c_str());
        back_delegate->OnTapDelegateBackEvent(tap);
    }
    else
    {
        DEFAULT_LOG_WARN("TAP 回传链为空，无法写入响应，TAP:{}", (void*)tap);
        tap->Drop("back chain empty");
    }
}

// ============================================================================
// TAP 通用异步操作（可在任意线程调用，内部回投到 libevent 线程）
// ============================================================================

void NetDock::ResponseAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResponse)
{
    std::string rspJson = jsResponse.dump();

    bool scheduled = ScheduleTaskInLoop(
        [this, tap, rspJson](void*) {
            // 校验 TAP 是否仍然存活（可能在异步处理期间被 Drop）
            if (tap->state != ZM_TAP_STATE_INUSE)
            {
                DEFAULT_LOG_WARN("TAP 已失效，丢弃异步响应，TAP:{}, state:{}",
                    (void*)tap, tap->state);
                return;
            }

            ZMJSON js;
            std::string err;
            js = zm_json_parse(rspJson, err);
            if (!err.empty())
            {
                DEFAULT_LOG_ERROR("异步响应 JSON 解析失败: {}，TAP:{}", err, (void*)tap);
                tap->Drop("async response json parse error");
                return;
            }
            Response(tap, js);
        },
        nullptr, nullptr);

    if (!scheduled)
    {
        DEFAULT_LOG_ERROR("ScheduleTaskInLoop 调度失败，事件循环可能已停止，TAP:{}", (void*)tap);
    }
}

void NetDock::SetDropTimerAsync(ZM_TAP_CTX* tap, int seconds, int micros, uint32_t drop_timeout_error_code)
{
    bool scheduled = ScheduleTaskInLoop(
        [tap, seconds, micros, drop_timeout_error_code](void*) {
            ZmTapContext::SetDropTimer(tap, seconds, micros, drop_timeout_error_code);
        },
        nullptr, nullptr);

    if (!scheduled)
    {
        DEFAULT_LOG_ERROR("ScheduleTaskInLoop 调度失败，SetDropTimerAsync 未执行，TAP:{}", (void*)tap);
    }
}
