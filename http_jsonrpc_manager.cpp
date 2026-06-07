#include "http_jsonrpc_manager.h"

#include "name_define.h"
#include "zm_net_socket.h"
#include "zm_logger.h"

#include <mutex>
#include <condition_variable>

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

/**
 * @brief 通过 bufferevent pair 向 Hub Proxy 交付 JRPC 请求并等待响应
 *
 * 使用 evutil_socketpair 创建一对互联 socket，替代原来的 TCP 短连接：
 *   fd[0] — Worker 线程端：写入请求、读取响应
 *   fd[1] — 事件循环线程端：注入 Hub Proxy，按正常 TAP 管道处理
 *
 * 相比 TCP 短连接的收益：
 *   1. 无 TCP 三次握手 / 四次挥手
 *   2. 无端口耗尽风险（不走 listen backlog）
 *   3. 数据在 kernel socket buffer 中传递，零网络栈开销
 */
bool HttpJsonRpcManager::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
    if (!m_scheduleFn)
    {
        DEFAULT_LOG_ERROR("Schedule function not set, cannot deliver request");
        return false;
    }

    // 1. 创建 socket pair（内存级互联）
    evutil_socket_t fd[2];
    if (evutil_socketpair(AF_INET, SOCK_STREAM, 0, fd) < 0)
    {
        DEFAULT_LOG_ERROR("evutil_socketpair failed");
        return false;
    }

    // 2. Worker 端：发送 JRPC 请求帧到 fd[0]（数据进入 kernel buffer，等待 fd[1] 读取）
    char             head[8] = { 'J', 'R', 'P', 'C', '\x0', '\x0', '\x0', '\x0' };
    ZM_EXT_TLV_HEAD* msgh = ((ZM_EXT_TLV_HEAD*)head);
    uint32_t         qlen = (uint32_t)strlen(reqjs);
    msgh->len = (uint32_t)htonl(qlen);

    if (send(fd[0], head, 8, 0) < 0 || send(fd[0], reqjs, qlen, 0) < 0)
    {
        DEFAULT_LOG_ERROR("bufferevent pair send request failed");
        evutil_closesocket(fd[0]);
        evutil_closesocket(fd[1]);
        return false;
    }

    // 3. 在事件循环线程中为 fd[1] 创建 TAP 并注入 Hub Proxy
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    tapReady = false;

    bool scheduled = m_scheduleFn(
        [fd_1 = fd[1], this, &mtx, &cv, &tapReady](void*) {
            ZmTapContextEventHandler::OnPairAcceptConn(m_evbase, m_tapContext, m_tapHubProxy, fd_1);
            {
                std::lock_guard<std::mutex> lock(mtx);
                tapReady = true;
            }
            cv.notify_one();
        },
        nullptr, nullptr);

    if (!scheduled)
    {
        DEFAULT_LOG_ERROR("Schedule to event loop failed, event loop not running");
        evutil_closesocket(fd[0]);
        evutil_closesocket(fd[1]);
        return false;
    }

    // 等待 TAP 设置完成（事件循环线程处理完 fd[1] 注入后通知）
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&tapReady] { return tapReady; });
    }

    // 4. Worker 端：从 fd[0] 读取响应（阻塞等待事件循环线程写入 fd[1] 的数据到达）
    uint32_t rsp_len = 0;
    if (4 != recv(fd[0], (char*)&rsp_len, 4, MSG_WAITALL))
    {
        DEFAULT_LOG_ERROR("Receive response header failed");
        evutil_closesocket(fd[0]);
        return false;
    }

    rsp_len = ntohl(rsp_len);
    if (rsp_len > ZM_BUF_SIZE_4M)
    {
        DEFAULT_LOG_ERROR("Response too large: {} bytes (max {})", rsp_len, (size_t)ZM_BUF_SIZE_4M);
        evutil_closesocket(fd[0]);
        return false;
    }

    ZmByteBuffer buf(rsp_len);
    if ((int)rsp_len != recv(fd[0], (char*)buf.Head(), rsp_len, MSG_WAITALL))
    {
        DEFAULT_LOG_ERROR("Receive response body failed");
        evutil_closesocket(fd[0]);
        return false;
    }

    // Worker 端关闭 fd[0]，事件循环端 fd[1] 由 BEV_OPT_CLOSE_ON_FREE 随 bufferevent 释放
    evutil_closesocket(fd[0]);

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
