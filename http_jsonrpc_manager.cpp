#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "zm_logger.h"

#include <mutex>
#include <condition_variable>

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_httpServerJRPC(nullptr)
    , m_hubMgr(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();
}

bool HttpJsonRpcManager::Open(HubProxyManager* hubMgr)
{
    if (!hubMgr)
        return false;

    m_hubMgr = hubMgr;

    // 创建 HTTP JSON-RPC 服务器
    if (nullptr == m_httpServerJRPC)
    {
        m_httpServerJRPC = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT);
        m_httpServerJRPC->Start();
        m_httpServerJRPC->SetJsonRpcCBEx(std::bind(&HttpJsonRpcManager::OnHttpJsonrpcEx, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    }

    return (nullptr != m_httpServerJRPC);
}

void HttpJsonRpcManager::Close()
{
    if (m_httpServerJRPC)
    {
        m_httpServerJRPC->Stop();
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }

    m_hubMgr = nullptr;
}

/**
 * @brief 通过 bufferevent pair 向 Hub Proxy 交付 JRPC 请求并等待响应
 *
 * 使用 evutil_socketpair 创建一对互联 socket，在 Worker 线程中写入请求，
 * 通过 ScheduleFn 在事件循环线程中调用 OnPairAcceptConn 注入 Hub Proxy。
 * Worker 线程阻塞等待响应通过 pair 的另一端返回。
 */
bool HttpJsonRpcManager::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
    if (!m_scheduleFn)
    {
        DEFAULT_LOG_ERROR("Schedule function not set, cannot deliver request");
        return false;
    }

    if (!m_hubMgr)
    {
        DEFAULT_LOG_ERROR("HubProxyManager not set, cannot deliver request");
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
    char             head[8] = { 0 };
    ZM_EXT_TLV_HEAD* msgh = ((ZM_EXT_TLV_HEAD*)head);
    memcpy(msgh->tag, ZM_JRPC_MAGIC, 4);
    uint32_t         qlen = (uint32_t)strlen(reqjs);
    msgh->len = (uint32_t)htonl(qlen);

    if (send(fd[0], head, 8, 0) < 0 || send(fd[0], reqjs, qlen, 0) < 0)
    {
        DEFAULT_LOG_ERROR("bufferevent pair send request failed");
        evutil_closesocket(fd[0]);
        evutil_closesocket(fd[1]);
        return false;
    }

    // 3. 在事件循环线程中将 fd[1] 注入 Hub Proxy
    //    捕获 hubMgr 裸指针而非 this，避免 HttpJsonRpcManager 先于 lambda 被释放导致 UAF
    //    生命周期保证：NetDock::UnInit 中 CloseHub 先于 CloseHttpJsonRpcServer 执行，
    //    Hub 关闭时释放 TAP → 关闭 fd[1] → Worker 端 recv(fd[0]) 收到 EOF 返回
    auto* hubMgr = m_hubMgr;
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    tapReady = false;

    bool scheduled = m_scheduleFn(
        [fd_1 = fd[1], hubMgr, &mtx, &cv, &tapReady](void*) {
            ZmTapContextEventHandler::OnPairAcceptConn(hubMgr->HubProxy(), fd_1);
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

    // 用 Size() 构造避免 std::string(const char*) 内部的 strlen 扫描
    rspjs = std::string(buf.Str(), buf.Size());
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
        error["code"] = ZM_JRPC_ERR_SEND_HUB;
        error["message"] = "SendToHubProxy error";
        return ZM_JRPC_ERR_SEND_HUB;
    }

    if (repjstr.empty())
    {
        error["code"] = ZM_JRPC_ERR_EMPTY_RSP;
        error["message"] = "Response is empty";
        return ZM_JRPC_ERR_EMPTY_RSP;
    }

    std::string jerrstr;
    ZMJSON repjson = zm_json_parse(repjstr, jerrstr);
    if (!repjson.is_object())
    {
        error["code"] = ZM_JRPC_ERR_FORMAT;
        error["message"] = "Response format error";
        return ZM_JRPC_ERR_FORMAT;
    }

    if (repjson["result"].is_object())
        result = repjson["result"];
    if (repjson["error"].is_object())
        error = repjson["error"];

    return code;
}
