#include "service_portal.h"

#include "net_dock.h"

#include "zm_net_http_frontend_server.h"
#include "zm_net_http_jsonrpc_server.h"
#include "zm_net_http_restful_server.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "zm_util_json.h"   // ZMJSON

#include <chrono>
#include <filesystem>
#include <thread>

#include "zm_util_logger.h"

using namespace drogon;
using std::string;

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================

ServicePortal::ServicePortal(NetDock* netDock)
    : m_netDock(netDock)
{
}

ServicePortal::~ServicePortal()
{
}

void ServicePortal::Init()
{
    if (!m_netDock)
    {
        DEFAULT_LOG_ERROR("ServicePortal::Init: NetDock 为空");
        return;
    }
    m_frontend = m_netDock->GetFrontendServer();
    m_jrpc = m_netDock->GetJsonRpcServer();
    m_restful = m_netDock->GetRestfulServer();

    DEFAULT_LOG_INFO("Portal::Init 前端");
    RegisterFrontendRoutes(m_frontend);
    DEFAULT_LOG_INFO("Portal::Init JRPC");
    RegisterJsonRpcRoutes(m_jrpc);
    DEFAULT_LOG_INFO("Portal::Init REST");
    RegisterRestfulTestRoutes(m_restful);
    DEFAULT_LOG_INFO("Portal::Init CORS");    // 预检+CORS 挂在 RESTful 面(业务层显式,FR-21;设计 §11.4)
    RegisterRestfulCors(m_restful);
    DEFAULT_LOG_INFO("Portal::Init 完成");
}

void ServicePortal::Shutdown()
{
    // 本期无业务线程;业务期在此先 join 业务线程(FR-04)
}

bool ServicePortal::BroadcastMessage(const string& topic, const string& content,
                                     const string& tag)
{
    (void)topic; (void)content; (void)tag;
    DEFAULT_LOG_INFO("BroadcastMessage: 广播服务(39640)本期未接入");
    return false;
}

// ============================================================================
// 前端页面路由(80/443;页面别名按旧迁移表,设计 §11.2)
// ============================================================================

/// 由文档根 + 相对路径构造静态页响应
/// 经 SendFileCoro(方案甲):自动获得 Range/206/416 与 Accept-Ranges(页面大图/
/// JS 断点续传、视频 seek 收益);缺页仍走自定义 404 页(HTML,保持前端语义),
/// 区别于 SendFileCoro 自身的 JSON 404 错误包。
ZmHttpCoroHandler MakePageHandler(ZmHttpFrontendServer* fe, const string& www,
                                 const string& relPath)
{
    return [fe, www, relPath](HttpRequestPtr req) -> Task<HttpResponsePtr> {
        string full = www;
        if (!full.empty() && full.back() != '\\' && full.back() != '/')
            full += "\\";
        full += relPath;
        // 缺页:走全局 setCustom404Page 的 HTML 404 页(而非 SendFileCoro 的 JSON 404)
        if (!std::filesystem::exists(full))
            co_return HttpResponse::newNotFoundResponse();
        co_return co_await fe->SendFileCoro(req, full);
    };
}

void ServicePortal::RegisterFrontendRoutes(ZmHttpFrontendServer* fe)
{
    if (!fe)
        return;
    const string& www = fe->GetDocumentRoot();

    // 逐条页面别名(旧前端路由表)
    fe->RegisterCoro("/", Get, MakePageHandler(fe, www, "html\\index.html"));
    fe->RegisterCoro("/login", Get, MakePageHandler(fe, www, "html\\login.html"));
    fe->RegisterCoro("/register", Get, MakePageHandler(fe, www, "html\\register.html"));
    fe->RegisterCoro("/reset", Get, MakePageHandler(fe, www, "html\\reset.html"));
    fe->RegisterCoro("/force-reset", Get, MakePageHandler(fe, www, "html\\force-reset.html"));
    fe->RegisterCoro("/404", Get, MakePageHandler(fe, www, "html\\404.html"));

    // /share/{token} 302 → RESTful 端口分享页(设计 §11.2;目标 URL 业务期再核定)
    const bool httpsMode = fe->IsHttps();
    fe->RegisterCoro("/share/{1}", Get,
        [this, httpsMode](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            // via-regex 路由:捕获组经 getRoutingParameters() 获取
            const auto& params = req->getRoutingParameters();
            string token = params.empty() ? "" : params[0];
            string host = req->getHeader("Host");
            size_t colon = host.rfind(':');
            if (colon != string::npos && host.find(']') == string::npos)
                host = host.substr(0, colon);
            // 分享目标:rest 面根路径 + /share/{token}
            string target = m_restful->GetRootPath() + "/share/" + token;
            string loc = (httpsMode ? "https://" : "http://") + host + ":39441" + target;
            co_return HttpResponse::newRedirectionResponse(loc);
        });

    // ── 前端面可配置结构(业务层提供具体路径;平台层只给机制,见 zm_net_http_frontend_server) ──
    // SPA 回落:当前 www 下无物理 /portal 目录,全部回落 portal.html(与旧 PortalModule 语义等价)
    fe->AddSpaFallback("/portal", "html/portal.html");
    // /doc 目录物理存在于 www 下,显式封禁(验收:不可达)
    fe->AddDeniedPath("/doc");
}

// ============================================================================
// JSON-RPC(39440):协议校验与信封由平台面内建(zm_net_http_jsonrpc_server),
// 业务层只注册 method 处理器;ping 为平台内建,无需注册。
// ============================================================================
void ServicePortal::RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc)
{
    if (!jrpc)
        return;

    // 演示业务注册形态:echo(原样回显 params;非平台内建)
    jrpc->RegisterMethod("echo",
        [](const ZMJSON& params, ZMJSON& result, ZMJSON& error) {
            (void)error;
            result["params"] = params;
            return true;
        });
}

// ============================================================================
// RESTful 测试接口(39441 /zimo/api;本期仅测试,不对接业务)
// ============================================================================
void ServicePortal::RegisterRestfulTestRoutes(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // 根路径可自定义(service_define.h 宏默认);测试路由统一以根前缀注册
    const string& rpcRoot = rest->GetRootPath();

    // 系统 ping(与旧版 RESTful 系统路由一致;基类默认 /ping 为 FR-23 全局健康检查)
    rest->RegisterCoro(rpcRoot + "/ping", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            ZMJSON d;
            d["pong"] = true;
            co_return ZmHttpServer::JsonResponse(200, d);
        });

    // 基本 JSON / 错误包
    rest->RegisterCoro(rpcRoot + "/test/json", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            ZMJSON d;
            d["name"] = "ZmHttpServer";
            d["test"] = true;
            co_return ZmHttpServer::JsonResponse(200, d);
        });

    rest->RegisterCoro(rpcRoot + "/test/error", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            co_return ZmHttpServer::ErrorResponse(500, "internal server error");
        });

    // 路径参数 / query 参数
    rest->RegisterCoro(rpcRoot + "/test/echo/{1}", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            const auto& params = req->getRoutingParameters();
            ZMJSON d;
            d["path"] = params.empty() ? "" : ZMJSON(params[0]);
            d["q"] = req->getParameter("q");
            d["method"] = req->getMethodString();
            co_return ZmHttpServer::JsonResponse(200, d);
        });

    // JSONP(FR-24)
    rest->RegisterCoro(rpcRoot + "/test/jsonp", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            ZMJSON d;
            d["pong"] = true;
            co_return ZmHttpServer::JsonpResponse(req, d);
        });

    // 文件传输(FR-12 方案甲 + Range;取 www/html/portal.html 作为样本)
    rest->RegisterCoro(rpcRoot + "/test/download", Get,
        [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            string www = m_netDock->GetFrontendServer()->GetDocumentRoot();
            string full = www;
            if (!full.empty() && full.back() != '\\' && full.back() != '/')
                full += "\\";
            full += "html\\portal.html";
            co_return co_await m_restful->SendFileHybridCoro(req, full, "portal.html");
        });

    // 文件传输(FR-12 方案乙 + Range;样本 = www/__selftest/blob.bin(测试路由,业务期替换))
    rest->RegisterCoro(rpcRoot + "/test/download-stream", Get,
        [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            string www = m_netDock->GetFrontendServer()->GetDocumentRoot();
            string full = www;
            if (!full.empty() && full.back() != '\\' && full.back() != '/')
                full += "\\";
            full += "__selftest\\blob.bin";
            ZmHttpSendFileOptions opts;
            opts.interBlockMs = 5;   // 测试:块间快速推进
            co_return co_await m_restful->SendFileStreamCoro(req, full, "blob.bin", opts);
        });

    // 文件传输(FR-12 方案乙验证):exe 同级 modules\filehub\0\ 下的 3.52GB iso
    // (≥2GB 阈值 → SendFileStreamCoro,HttpIoPool 分块流式;临时测试口,验证后删)
    rest->RegisterCoro(rpcRoot + "/test/download-iso", Get,
        [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            string full = ZmExeDir() + "modules\\filehub\\0\\"
                          "cn_office_professional_plus_2019_x86_x64_dvd_5e5be643.iso";
            co_return co_await m_restful->SendFileHybridCoro(
                req, full, "cn_office_professional_plus_2019_x86_x64_dvd_5e5be643.iso");
        });

    // 业务 deadline(FR-14):睡眠 3s > deadline 1s → 504 且只回一次
    rest->RegisterCoroWithDeadline(rpcRoot + "/test/slow", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            co_await ZmHttpServer::RunOnPool<int>([]() -> int {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                return 0;
            });
            ZMJSON d;
            d["slow"] = false;
            co_return ZmHttpServer::JsonResponse(200, d);
        }, 1000);

    // 阻塞离核演示(FR-19):PBKDF2 模拟(实质为 sleep,返回计时结果)
    rest->RegisterCoro(rpcRoot + "/test/pool", Get,
        [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
            auto started = std::chrono::steady_clock::now();
            int r = co_await ZmHttpServer::RunOnPool<int>([]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                return 42;
            });
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started).count();
            ZMJSON d;
            d["value"] = r;
            d["usedPool"] = static_cast<int64_t>(ms) >= 0;
            co_return ZmHttpServer::JsonResponse(200, d);
        });

    // 流式上传演示(FR-15 路径 B):路由级 10GB 上限经注册参数 maxBytes(基类自动
    // X-File-Size 早拒);块到即写系统临时目录;上限经 attributes 取出做落盘兜底
    rest->RegisterStreamCoro(rpcRoot + "/test/upload-stream", Post,
        [](HttpRequestPtr req, RequestStreamPtr stream) -> Task<HttpResponsePtr> {
            // 上限由注册参数内置(基类已做早拒);经 attributes 取出,用于落盘兜底
            uint64_t kUploadMax = 10ULL * 1024 * 1024 * 1024;
            try { kUploadMax = req->getAttributes()->get<uint64_t>("ZmStreamMaxBytes"); }
            catch (...) {}

            std::error_code ec;
            string dest = (std::filesystem::temp_directory_path() /
                           "zmsvc_upload_received.bin").string();
            bool tooLarge = false;
            ZmHttpUploadFileOptions opts;
            opts.maxBytes = kUploadMax;
            bool ok = co_await ZmHttpServer::SaveStreamToFile(
                std::move(stream), dest, opts, &tooLarge);

            ZMJSON d;
            if (!ok)
            {
                d["ok"] = false;
                d["tooLarge"] = tooLarge;
                d["dest"] = dest;
                co_return ZmHttpServer::JsonResponse(tooLarge ? 413 : 400, d);
            }
            d["ok"] = true;
            d["size"] = static_cast<uint64_t>(std::filesystem::file_size(dest, ec));
            co_return ZmHttpServer::JsonResponse(200, d);
        },
        {}, 10ULL * 1024 * 1024 * 1024);   // 路由级上限:10GB

    // WebSocket echo(FR-16)
    ZmHttpServer::WsCallbacks wsCb;
    wsCb.onAuth = [](HttpRequestPtr req) {
        // 本期测试:放行(业务期在此接 AuthAndTouch 会话校验)
        return true;
    };
    wsCb.onOpen = [](const WebSocketConnectionPtr& conn, const HttpRequestPtr& req) {
        conn->send("welcome");
    };
    wsCb.onMessage = [](const WebSocketConnectionPtr& conn, string&& msg, WebSocketMessageType type) {
        conn->send("echo:" + msg, type);
    };
    wsCb.onClose = [](const WebSocketConnectionPtr& conn) {
        DEFAULT_LOG_INFO("ws 连接关闭");
    };
    rest->RegisterWebSocket(rpcRoot + "/test/ws", wsCb);
}

void ServicePortal::RegisterRestfulCors(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // OPTIONS 预检(FR-21)
    rest->RegisterPreRouting([](const HttpRequestPtr& req, AdviceCallback&& cb,
                                AdviceChainCallback&& cc) {
        if (req->method() != Options)
        {
            cc();
            return;
        }
        ZMJSON d;
        auto resp = ZmHttpServer::JsonResponse(200, d);
        string origin = req->getHeader("Origin");
        if (!origin.empty())
        {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Access-Control-Allow-Credentials", "true");
        }
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Origin, Content-Type, Accept, X-File-Size");
        cb(resp);
    });

    // 响应附加 CORS 头(回显 Origin + 凭据)
    rest->RegisterPreSending([](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
        if (req->method() == Options)
            return;   // 预检响应已带
        string origin = req->getHeader("Origin");
        if (!origin.empty() &&
            (req->getHeader("cookie").find("zm_session") != string::npos ||
             resp->getStatusCode() >= k200OK))
        {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Access-Control-Allow-Credentials", "true");
        }
    });
}
