//#include "http_server.h"
//#include "name_define.h"
//
//enum
//{
//    TERM_TYPE_NAMEDPIPE = 0,
//    TERM_TYPE_HTTP = 1,
//    TERM_TYPE_WEBSOCKET = 2
//};
//
//bool Trust_JRPCViaNetwork(const char* reqjs, std::string& rspjs)
//{
//    //ZmNetSocketTCP conn;
//    //if (conn.Open("127.0.0.1", ZmGetPort(NULL, 0)))
//    //{
//    //    char             head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
//    //    Y_EXT_TLV_HEAD* msgh = ((Y_EXT_TLV_HEAD*)head);
//    //    uint32_t         qlen = (uint32_t)strlen(reqjs);
//    //    msgh->len = (uint32_t)htonl(qlen);
//    //    conn.Send(head, 8);
//    //    conn.Send(reqjs, qlen);
//    //    qlen = 0;
//    //    if (4 == conn.Recv(&qlen, 4))
//    //    {
//    //        YByteBuffer buf(ntohl(qlen));
//    //        size_t offset = 0;
//    //        while (offset < buf.Size())
//    //        {
//    //            int rlen = conn.Recv(buf.Head(offset), buf.Size() - offset);
//    //            if (rlen > 0)
//    //            {
//    //                offset += rlen;
//    //            }
//    //            else
//    //            {
//    //                //Y_LOGT("%s Recv return=%d, failed: %s", __Y_FUNC__, rlen, YErrMsg(0));
//    //                break;
//    //            }
//    //        }
//    //        rspjs = std::string(buf.Str());
//    //    }
//    //}
//    //else
//    //{
//    //    //Y_LOGT("%s Connect failed: %s", __Y_FUNC__, YErrMsg(0));
//    //}
//    //return !rspjs.empty();
//
//    return true;
//}
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
//        if (ZmString::StartsWith(method.c_str(), "trust_"))
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
//void OnTermRequest(const ZMJSON& request, ZMJSON& reply, int termType)
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
//int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method, const ZMJSON& params, ZMJSON& result, ZMJSON& error)
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
//
//
//HttpServer::HttpServer()
//{
//
//}
//
//HttpServer::~HttpServer()
//{
//
//}
//
//void HttpServer::Start()
//{
//    if (nullptr == m_http_server)
//    {
//        m_http_server = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, 39440);
//        m_http_server->Start();
//
//        m_http_server->SetJsonRpcCBEx(OnHttpJsonrpcEx);
//    }
//}
//
//void HttpServer::Stop()
//{
//    m_http_server->Stop();
//}