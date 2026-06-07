#include "http_jsonrpc_manager.h"

#include "name_define.h"
#include "zm_net_socket.h"
#include "zm_logger.h"

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_tapContext(nullptr)
    , m_tapDelegateJRPC(nullptr)
    , m_tapHubProxy(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_hubProxyPort(0)
    , m_evbase(nullptr)
    , m_evdnsbase(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();
}

bool HttpJsonRpcManager::Open(event_base* evbase, evdns_base* evdnsbase,
                              TapDelegateJrpcRequestReadCB cb)
{
    if (!evbase)
        return false;

    m_evbase = evbase;
    m_evdnsbase = evdnsbase;

    // 1. 创建 JRPC 协议委托处理器
    if (nullptr == m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC = new ZmTapDelegateJRPC();
        m_tapDelegateJRPC->SetEvDns(m_evdnsbase);
        m_tapDelegateJRPC->StartTapDelegate(m_evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_JRPC);
        m_tapDelegateJRPC->SetJrpcRequestReadCB(cb);
    }

    // 2. 创建 TAP 上下文池和 Hub 代理
    if (nullptr == m_tapHubProxy)
    {
        if (nullptr == m_tapContext)
        {
            m_tapContext = new ZmTapContext();
        }

        m_tapHubProxy = new ZmTapHubProxy();
        m_tapHubProxy->SetJrpcDelegate(m_tapDelegateJRPC);
        m_tapHubProxy->SetEvDns(m_evdnsbase);
        m_tapHubProxy->StartTapDelegate(m_tapContext, m_evbase, ZM_DELEGATE_MODE_PROXY_INTERNAL_HUB);
        m_hubProxyPort = m_tapHubProxy->AddListenPort(0);
    }

    // 3. 创建 HTTP JSON-RPC 服务器
    if (m_hubProxyPort && (nullptr == m_httpServerJRPC))
    {
        m_httpServerJRPC = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, 39440);
        m_httpServerJRPC->Start();

        m_httpServerJRPC->SetJsonRpcCBEx(std::bind(&HttpJsonRpcManager::OnHttpJsonrpcEx, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    }

    return (nullptr != m_httpServerJRPC);
}

void HttpJsonRpcManager::Close()
{
    // 释放顺序与创建顺序相反
    if (m_httpServerJRPC)
    {
        m_httpServerJRPC->Stop();
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }

    if (m_tapHubProxy)
    {
        m_tapHubProxy->StopTapDelegate();
        delete m_tapHubProxy;
        m_tapHubProxy = nullptr;
    }

    if (m_tapDelegateJRPC)
    {
        m_tapDelegateJRPC->StopTapDelegate();
        delete m_tapDelegateJRPC;
        m_tapDelegateJRPC = nullptr;
    }

    if (m_tapContext)
    {
        m_tapContext->Clear();
        delete m_tapContext;
        m_tapContext = nullptr;
    }

    m_hubProxyPort = 0;
    m_evbase = nullptr;
    m_evdnsbase = nullptr;
}

/** 通过 TCP 短连接向本地 Hub Proxy 发送 JRPC 请求并等待响应 */
bool HttpJsonRpcManager::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
    if (0 == m_hubProxyPort)
    {
        DEFAULT_LOG_ERROR("HubProxy port not ready");
        return false;
    }

    ZmNetSocketTCP conn;
    const char* host = "127.0.0.1";

    if (!conn.Open(host, m_hubProxyPort))
    {
        DEFAULT_LOG_ERROR("Connect HubProxy failed");
        return false;
    }

    // 8字节数据帧格式，用于扩展 sock5
    char             head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
    ZM_EXT_TLV_HEAD* msgh = ((ZM_EXT_TLV_HEAD*)head);
    uint32_t         qlen = (uint32_t)strlen(reqjs);
    msgh->len = (uint32_t)htonl(qlen);

    if (conn.Send(head, 8) < 0 || conn.Send(reqjs, qlen) < 0)
    {
        DEFAULT_LOG_ERROR("Socket send failed");
        return false;
    }

    /** 使用 RecvAll 循环读取防止 TCP 分包导致只读到部分长度头 */
    uint32_t rsp_len = 0;
    if (4 != conn.RecvAll(&rsp_len, 4))
    {
        DEFAULT_LOG_ERROR("Receive response header failed");
        return false;
    }

    /** 使用 RecvAll 循环读取防止 TCP 分包导致只读到部分响应体 */
    rsp_len = ntohl(rsp_len);
    if (rsp_len > ZM_BUF_SIZE_4M)
    {
        DEFAULT_LOG_ERROR("Response too large: {} bytes (max {})", rsp_len, (size_t)ZM_BUF_SIZE_4M);
        return false;
    }
    ZmByteBuffer buf(rsp_len);
    if ((int)rsp_len != conn.RecvAll(buf.Head(), rsp_len))
    {
        DEFAULT_LOG_ERROR("Receive response body failed");
        return false;
    }

    rspjs = std::string(buf.Str());
    return !rspjs.empty();
}

int HttpJsonRpcManager::OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
                                        const ZMJSON& params, ZMJSON& result, ZMJSON& error)
{
    int code = 0;

    ZMJSON reqobj;
    reqobj["method"] = method;
    reqobj["ip"] = task->Ip();
    reqobj["port"] = task->Port();
    reqobj["request_useragent"] = task->GetRequestHeader("User-Agent");
    reqobj["params"] = params;

    std::string repjstr;
    if (!SendToHubProxy(reqobj.dump().c_str(), repjstr))
    {
        error["code"] = 601;
        error["message"] = "SendToHubProxy error";
        return 601;
    }

    if (repjstr.empty())
    {
        error["code"] = 602;
        error["message"] = "Response is empty";
        return 602;
    }

    std::string jerrstr;
    ZMJSON repjson = zm_json_parse(repjstr, jerrstr);
    if (!repjson.is_object())
    {
        error["code"] = 603;
        error["message"] = "Response format error";
        return 603;
    }

    if (repjson["result"].is_object())
    {
        result = repjson["result"];
    }
    if (repjson["error"].is_object())
    {
        error = repjson["error"];
    }

    return code;
}
