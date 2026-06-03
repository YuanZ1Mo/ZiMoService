//#include "control_module.h"
////#include "zm_net_socket.h"
////#include "y_util_str.h"
//
//#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
//#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))
//
//
////#include <unordered_set>
//
//std::unique_ptr<YServiceMessageModule> g_y_service_message_module;
//std::unique_ptr<YCommunicateCtrlModule> g_y_communicate_ctrl_module;
//
//#ifdef __cplusplus
//extern "C"
//{
//#endif
//    void(__stdcall* g_y_service_async_callback)(const char* topic, const char* contentjs, bool bNotifyUpMessage) = NULL;
//#ifdef __cplusplus
//}
//#endif
//
//class YNotifyThread : public YThread
//{
//public:
//    YNotifyThread(const char* topic, std::string& contentjs, bool bNotifyUpMessage, int delay_mills = 0)
//        : YThread("YNotifyThread", true), _topic(topic), _bNotifyUpMessage(bNotifyUpMessage), _delay_mills(delay_mills)
//    {
//        _contentjs = std::move(contentjs);
//    }
//
//    ~YNotifyThread()
//    {}
//
//protected:
//    virtual void Run()
//    {
//        if (NULL != g_y_service_async_callback)
//        {
//            if (_delay_mills > 0)
//            {
//                ZmSleepMS(_delay_mills);
//            }
//            g_y_service_async_callback(_topic.c_str(), _contentjs.c_str(), _bNotifyUpMessage);
//        }
//    }
//
//
//private:
//    std::string     _topic;
//    std::string     _contentjs;
//    bool            _bNotifyUpMessage;
//    int             _delay_mills;
//};
//
//
//void YServiceMessageModule::InitializeCB(void(__stdcall* callback)(const char* topic, const char* contentjs, bool bNotifyUpMessage))
//{
//    if (callback)
//    {
//        g_y_service_async_callback = callback;
//    }
//}
//
//void YServiceMessageModule::NotifyMessage(const char* topic, const ZMJSON& content, bool bNotifyUpMessage, int delay_mills)
//{
//    if (g_y_service_async_callback)
//    {
//        std::string jstr = ZMJSON(content).dump();
//
//        YNotifyThread* thread = new YNotifyThread(topic, jstr, bNotifyUpMessage, delay_mills);
//        thread->Begin();
//    }
//}
//
//YCommunicateCtrlModule::YCommunicateCtrlModule() :
//    _http_server(Y_LOCAL_HTTPSERVER_NAME, 39440)
//{
//    //while (true)
//    //{
//    //    Sleep(1000);
//    //}
//
//    //aaa = YThread2<MyThread>::CreateSharedPtr<MyThread>("asd");
//    //YThread_shared_ptr<MyThread> aaa = YThread2<MyThread>::CreateSharedPtr<MyThread>("asd");
//    //aaa->SetAutoDel(true);
//    //aaa->Start();
//
//    //TODO 或者不设置?
//    //_http_server.SetRequestCallback(std::bind(&YCommunicateCtrlModule::OnHttpRequest, this,
//    //    std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
//    _http_server.SetJsonRpcCBEx(std::bind(&YCommunicateCtrlModule::OnHttpJsonrpcEx, this,
//        std::placeholders::_1, std::placeholders::_2,
//        std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
//
//    _MessageServer.SetBindPort(37310);
//}
//
//void YCommunicateCtrlModule::Start()
//{
//    if (!g_winsock_helper.Get())
//    {
//        g_winsock_helper.Set(new YWinSockHelper());
//    }
//
//    //_MessageServer.Start();
//    //_http_server.Startup();
//    //_pipe_server.Begin(); //有问题,退出时会卡住
//}
//
//
//void YCommunicateCtrlModule::Stop()
//{
//    //_MessageServer.Stop();
//    //_http_server.Shutdown();
//    //_pipe_server.Close();
//}
//
//void YCommunicateCtrlModule::PushNotify(const char* topic, const char* contentjs)
//{
//    _MessageServer.PostNotificationWithTopic(topic, contentjs);
//}
//
//int YCommunicateCtrlModule::OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen)
//{
//    int code = 404;
//    task->PutReplyHeader("Access-Control-Allow-Origin", "*");
//    if (0 == strcasecmp(task->Uri(), "/ping"))
//    {
//        code = 200;
//        task->SetReplyData((const BYTE*)"pong", 4);
//    }
//
//    // SPLogging::Flush(0);
//    return code;
//}
//
//int YCommunicateCtrlModule::OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method, const ZMJSON& params, ZMJSON& result, ZMJSON& error)
//{
//    int code = 0;
//    ZMJSON reqobj;
//    reqobj["method"] = method;
//
//    ZMJSON paramsobj = params;
//    std::string  useragent = zm_json_get_str(params, "browser_useragent");
//    if (useragent.empty())
//    {
//        paramsobj["browser_useragent"] = task->GetRequestHeader("User-Agent");
//    }
//    reqobj["params"] = paramsobj;
//
//    ZMJSON request = ZMJSON(reqobj);
//    ZMJSON reply;
//    OnTermRequest(request, reply, TERM_TYPE_HTTP);
//
//    if (reply["result"].is_object())
//    {
//        result = reply["result"];
//    }
//    if (reply["error"].is_object())
//    {
//        error = reply["error"];
//    }
//    return code;
//}
//
//void YCommunicateCtrlModule::OnTermRequest(const ZMJSON& request, ZMJSON& reply, int termType)
//{
//    std::string jerrstr;
//    std::string method = zm_json_get_str(request, "method");
//
//    // TODO 对想要关注的method进行处理
//    //method.compare("*****")
//
//    //TODO 对关注的method进行NotifyFetch
//    switch (termType)
//    {
//    case TERM_TYPE_NAMEDPIPE:
//        //_pipe_term.NotifyFetch(***);
//        break;
//    case TERM_TYPE_HTTP:
//        //_http_term.NotifyFetch(***);
//        break;
//    case TERM_TYPE_WEBSOCKET:
//        break;
//    default:
//        break;
//    }
//
//    std::string reqjstr = request.dump();
//    std::string repjstr;
//    int    errcode = Trust_JSONRpc(reqjstr.c_str(), repjstr);
//
//    ZMJSON repjson = zm_json_parse(repjstr, jerrstr);
//    if (!repjson.is_object())
//    {
//        //TODO 不是json的操作
//    }
//    else
//    {
//        reply = repjson;
//    }
//}
//
//
//int  Trust_JSONRpc(const char* reqjs, std::string& repjstr)
//{
//    int             errcode = 0;
//    std::string     errmsg;
//
//    ZMJSON    jresult;
//
//    std::string jerr;
//    std::string method;
//    ZMJSON rpcjson = zm_json_parse(reqjs, jerr);
//    if (jerr.empty())
//    {
//        method = zm_json_get_str(rpcjson, "method");
//
//        //TODO 目前支持所有请求方法
//        //if (YString::StartsWith(method.c_str(), "trust_"))
//        if (1)
//        {
//            if (!Trust_JRPCViaNetwork(reqjs, repjstr))
//            {
//                errcode = 599;
//                errmsg = "Trust_JRPCViaNetwork error";
//            }
//        }
//        else
//        {
//            // -404    Method not found
//            errcode = -404;
//            errmsg = "[AstraliserService]Method not found";
//        }
//    }
//    else
//    {
//        // -400    Invalid Request
//        errcode = -400;
//        errmsg = "[AstraliserService]Invalid Request";
//    }
//
//    if (repjstr.empty())
//    {
//        ZMJSON pobj;
//        pobj["method"] = method;
//        if (errcode)
//        {
//            pobj["error"] = ZMJSON{ {"code", errcode}, {"message", errmsg} };
//        }
//        else
//        {
//            pobj["result"] = jresult;
//        }
//        repjstr = ZMJSON(pobj).dump();
//    }
//    return errcode;
//}
//
//#include "y_net_runloop.h"
///** Dispatch JSONRpc into LibEvent main thread */
//bool Trust_JRPCViaNetwork(const char* reqjs, std::string& rspjs)
//{
//    YNetSocketTCP conn;
//
//    // 将一个socket连接到一个指定端口上, 这里是连接到反向代理的一个总线端口上,具体作用是做流量转发
//    // 每次http请求都是一个新的线程,由conn来做转发,将流量转发到总线(真正的服务端)上
//    if (conn.Open("127.0.0.1", YGetPort(NULL, 0)))
//    {
//        char            head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
//        Y_EXT_TLV_HEAD* msgh = ((Y_EXT_TLV_HEAD*)head);
//        uint32_t         qlen = (uint32_t)strlen(reqjs);
//        msgh->len = (uint32_t)htonl(qlen);
//
//        /*
//        * 在TCP协议中，发送的数据通常会被视为一个连续的流，而不是独立的消息。
//        * 因此，即使分两次发送数据，接收端可能会将它们作为一个连续的数据流接收。
//        * 这意味着，接收端可能会收到一个包含两个数据部分的整合数据，也就是 head 数组后面紧跟着 reqjs 字符串。
//        */
//        conn.Send(head, 8);
//        conn.Send(reqjs, qlen);
//        qlen = 0;
//        if (4 == conn.Recv(&qlen, 4))
//        {
//            ZmByteBuffer buf(ntohl(qlen));
//            size_t offset = 0;
//            while (offset < buf.Size())
//            {
//                int rlen = conn.Recv(buf.Head(offset), buf.Size() - offset);
//                if (rlen > 0)
//                {
//                    offset += rlen;
//                }
//                else
//                {
//                    ////Y_LOGI("%s Recv return=%d, failed: %s", __Y_FUNC__, rlen, YErrMsg(-1));
//                    break;
//                }
//            }
//            rspjs = std::string(buf.Str());
//        }
//    }
//    else
//    {
//        ////Y_LOGI("%s Connect failed: %s", __Y_FUNC__, YErrMsg(-1));
//    }
//    return !rspjs.empty();
//}