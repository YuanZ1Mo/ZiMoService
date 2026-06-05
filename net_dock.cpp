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


/** Dispatch JSONRpc into LibEvent main thread，复用持久连接减少 TCP 握手开销 */
bool NetDock::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
    std::lock_guard<std::mutex> lock(_hubConnMutex);

    /** 连接不存在或已断开时自动重连 */
    if (!_hubConn.IsConnected() && !_hubConn.Open("127.0.0.1", _hubProxyPort))
    {
        DEFAULT_LOG_ERROR("Connect HubProxy failed");
        return false;
    }

    //8字节数据帧格式,用于扩展sock5
    char             head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
    ZM_EXT_TLV_HEAD* msgh = ((ZM_EXT_TLV_HEAD*)head);
    uint32_t         qlen = (uint32_t)strlen(reqjs);
    msgh->len = (uint32_t)htonl(qlen);

    if (_hubConn.Send(head, 8) < 0 || _hubConn.Send(reqjs, qlen) < 0)
    {
        DEFAULT_LOG_ERROR("Socket send failed, closing persistent connection");
        _hubConn.Close();
        return false;
    }

    /** 使用 RecvAll 循环读取防止 TCP 分包导致只读到部分长度头 */
    uint32_t rsp_len = 0;
    if (4 != _hubConn.RecvAll(&rsp_len, 4))
    {
        DEFAULT_LOG_ERROR("Receive response header failed");
        _hubConn.Close();
        return false;
    }

    /** 使用 RecvAll 循环读取防止 TCP 分包导致只读到部分响应体 */
    rsp_len = ntohl(rsp_len);
    if (rsp_len > ZM_BUF_SIZE_4M)
    {
        DEFAULT_LOG_ERROR("Response too large: {} bytes (max {})", rsp_len, (size_t)ZM_BUF_SIZE_4M);
        _hubConn.Close();
        return false;
    }
    ZmByteBuffer buf(rsp_len);
    if ((int)rsp_len != _hubConn.RecvAll(buf.Head(), rsp_len))
    {
        DEFAULT_LOG_ERROR("Receive response body failed");
        _hubConn.Close();
        return false;
    }

    rspjs = std::string(buf.Str());
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
        _tapDelegate_JRPC->SetEvDns(_evdnsbase);
        _tapDelegate_JRPC->StartTapDelegate(_evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC);
        _tapDelegate_JRPC->SetJrpcRequestReadCB(_tapDelegateJrpcRequestReadCB);
    }

    if (nullptr == _tapHubProxy)
    {
        if (nullptr == _tapContext)
        {
            _tapContext = new ZmTapContext();
        }

        _tapHubProxy = new ZmTapHubProxy();
        _tapHubProxy->SetJrpcDelegate(_tapDelegate_JRPC);
        _tapHubProxy->SetEvDns(_evdnsbase);
        _tapHubProxy->StartTapDelegate(_tapContext, _evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
        _hubProxyPort = _tapHubProxy->AddListenPort(0);
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