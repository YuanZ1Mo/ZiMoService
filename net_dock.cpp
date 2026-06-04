#include "net_dock.h"

#include "name_define.h"
#include "zm_net_socket.h"
#include "zm_logger.h"

NetDock::NetDock()
{

}
NetDock::~NetDock()
{
    CloseWebSocketServer();

    CloseHttpServer();

    if (_dock_runloop)
    {
        _dock_runloop->Stop();
    }
}

void NetDock::Init()
{
    if (!_dock_runloop)
    {
        _dock_runloop = new DockRunLoop();
        if (_dock_runloop->Loop())
        {
            _evbase = _dock_runloop->GetEventBase();
            _evdnsbase = _dock_runloop->GetEventDnsBase();
        }
    }
}



void NetDock::OpenWebSocketServer()
{
    if (nullptr == _messageServer)
    {
        _messageServer = new ZmMessageServer();
        // 设置 WebSocket 广播服务监听端口为 37310
        _messageServer->SetBindPort(37310);
        // 启动 WebSocket 服务器，开始监听连接
        _messageServer->Start();
    }
}

void NetDock::CloseWebSocketServer()
{
    if (nullptr != _messageServer)
    {
        // 停止 WebSocket 服务器，关闭所有客户端连接
        _messageServer->Stop();
        delete _messageServer;
        _messageServer = nullptr;
    }
}


/** Dispatch JSONRpc into LibEvent main thread */
bool NetDock::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
    ZmNetSocketTCP conn;
    const char* host = "127.0.0.1";
    int headlen = 0;

    if (conn.Open(host, _hubProxyPort))
    {
        char             head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
        ZM_EXT_TLV_HEAD* msgh = ((ZM_EXT_TLV_HEAD*)head);
        uint32_t         qlen = (uint32_t)strlen(reqjs);
        msgh->len = (uint32_t)htonl(qlen);

        int sendCode = 0;
        sendCode = conn.Send(head, 8);
        sendCode = conn.Send(reqjs, qlen);

        if (sendCode < 0) {
            //conn.SetErrorCode(ZM_SOCKET_SEND_ERROR);
            DEFAULT_LOG_ERROR("Socket send error, sendCode={}", sendCode);
            return false;
        }

        qlen = 0;
        headlen = conn.Recv(&qlen, 4);

        if (4 == headlen)
        {
            ZmByteBuffer buf(ntohl(qlen));
            size_t offset = 0;
            while (offset < buf.Size())
            {
                int rlen = conn.Recv(buf.Head(offset), buf.Size() - offset);
                if (rlen > 0)
                {
                    offset += rlen;
                }
                else
                {
                    //conn.SetErrorCode(ZM_SOCKET_RECEIVE_ERROR);
                    DEFAULT_LOG_ERROR("Socket receive error, receiveCode={}", rlen);
                    break;
                }
            }
            rspjs = std::string(buf.Str());
        }
        else
        {
            DEFAULT_LOG_ERROR("Receive headlen error");
        }
    }
    else
    {
        DEFAULT_LOG_ERROR("Connect HubProxy failed");
    }

    return !rspjs.empty();
}

int NetDock::OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method, const ZMJSON& params, ZMJSON& result, ZMJSON& error)
{
    int code = 0;
    ZMJSON paramsObj = params;

    ZMJSON reqobj;
    reqobj["method"] = method;
    reqobj["ip"] = task->Ip();
    reqobj["port"] = task->Port();
    reqobj["request_useragent"] = task->GetRequestHeader("User-Agent");
    reqobj["params"] = paramsObj;

    int             errCode = 0;
    std::string     errMsg;
    std::string     repjstr;
    if (!SendToHubProxy(reqobj.dump().c_str(), repjstr))
    {
        errCode = 601;
        errMsg = "SendToHubProxy error";
    }

    if (repjstr.empty())
    {
        errCode = 602;
        errMsg = "Response is empty";
    }
    else
    {
        std::string jerrstr;
        ZMJSON repjson = zm_json_parse(repjstr, jerrstr);
        if (!repjson.is_object())
        {
            errCode = 603;
            errMsg = "Response format error";
        }
        else
        {
            if (repjson["result"].is_object())
            {
                result = repjson["result"];
            }
            if (repjson["error"].is_object())
            {
                error = repjson["error"];
            }
        }
    }

    return code;
}


void NetDock::OpenHttpServer()
{
    if (nullptr == _tapDelegate_JRPC)
    {
        _tapDelegate_JRPC = new ZmTapDelegateJRPC();
        _tapDelegate_JRPC->StartTapDelegate(nullptr, nullptr, nullptr, ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC);
        _tapDelegate_JRPC->SetJrpcRequsetReadCB(_tapDelegateJrpcRequsetReadCB);
    }

    if (nullptr == _tapDomainNameResolver)
    {
        _tapDomainNameResolver = new ZmTapDomainNameResolver(); 
    }

    if (nullptr == _tapHubProxy)
    {
        if (nullptr == _tapContext)
        {
            _tapContext = new ZmTapContext();
        }

        _tapHubProxy = new ZmTapHubProxy();
        _tapHubProxy->SetJrpcDelegate(_tapDelegate_JRPC);
        _tapHubProxy->SetDomainNameResolver(_tapDomainNameResolver);
        _tapHubProxy->StartTapDelegate(_tapContext, _evbase, _evdnsbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
        _hubProxyPort = _tapHubProxy->AddDummpy(0);
    }

    if (_hubProxyPort && (nullptr == _httpServer_JRPC))
    {
        _httpServer_JRPC = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, 39440);
        _httpServer_JRPC->Start();

        _httpServer_JRPC->SetJsonRpcCBEx(std::bind(&NetDock::OnHttpJsonrpcEx, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    }
}

void NetDock::CloseHttpServer()
{

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
    if (_dock_runloop->IsLooped())
    {
        ZM_SCHEDULE_CTX* ctx = new ZM_SCHEDULE_CTX();
        ctx->fn_run = fn_run;
        ctx->fn_cb = fn_cb;
        ctx->param = param;
        ctx->ev_schedule = event_new(_evbase,
            -1, EV_PERSIST | EV_READ, NetDock::OnSheculeEventCB, ctx);
        event_add(ctx->ev_schedule, NULL);
        event_active(ctx->ev_schedule, 0, 0);
        /** TODO: 需要将ctx存储起来，以便事件还未触发时可取消和删除 */
        return true;
    }
    return false;
}

void NetDock::OnSheculeEventCB(evutil_socket_t fd, short what, void* ctx)
{
    if (ctx)
    {
        ZM_SCHEDULE_CTX* schedule = (ZM_SCHEDULE_CTX*)ctx;
        if (schedule->ev_schedule)
        {
            event_free(schedule->ev_schedule);
        }
        if (schedule->fn_run)
        {
            schedule->fn_run(schedule->param);
        }
        if (schedule->fn_cb)
        {
            schedule->fn_cb(schedule->param);
        }
        delete schedule;
    }
}