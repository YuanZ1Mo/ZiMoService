#include "service_portal.h"

#include "net_dock.h"

#include "zm_net_http_frontend_server.h"
#include "zm_net_http_jsonrpc_server.h"
#include "zm_net_http_restful_server.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "zm_util_json.h"   // ZMJSON
#include "zm_util_logger.h"

// 业务模块
#include "modules/module_db.h"
#include "modules/module_user.h"
#include "modules/module_password.h"
#include "modules/module_session.h"
#include "modules/module_permission.h"
#include "modules/module_security.h"
#include "modules/module_audit.h"
#include "modules/module_gate.h"
#include "modules/module_auth.h"
#include "modules/module_user_admin.h"
#include "modules/module_portal.h"

using namespace drogon;
using std::string;

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================

ServicePortal::ServicePortal(NetDock* netDock)
    : m_netDock(netDock)
{
}

ServicePortal::~ServicePortal() = default;

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

    // 先创建业务模块(数据库建库建表种子;依赖注入)
    CreateModules();

    DEFAULT_LOG_INFO("Portal::Init 前端");
    RegisterFrontendRoutes(m_frontend);
    DEFAULT_LOG_INFO("Portal::Init JRPC");
    RegisterJsonRpcRoutes(m_jrpc);
    DEFAULT_LOG_INFO("Portal::Init REST");
    RegisterRestfulRoutes(m_restful);
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
// 业务模块装配(依赖方向:编排模块 → 数据服务模块 → DbModule;门禁最后装,先于接口注册)
// ============================================================================
void ServicePortal::CreateModules()
{
    // 数据访问(全新建库建表 + 种子;库文件 exe 同级 db\user.db)
    m_db = std::make_unique<ZmDbModule>();
    if (!m_db->Init(ZmExeDir() + "db\\user.db"))
    {
        DEFAULT_LOG_ERROR("ServicePortal::CreateModules: ZmDbModule::Init 失败,业务功能不可用");
    }
    else
    {
        m_db->StartPeriodicCleanup();
    }
    // 数据服务模块
    m_user = std::make_unique<ZmUserModule>(m_db.get());
    m_password = std::make_unique<ZmPasswordModule>(m_user.get());
    m_session = std::make_unique<ZmSessionModule>(m_db.get());
    m_permission = std::make_unique<ZmPermissionModule>(m_db.get(), m_user.get());
    m_security = std::make_unique<ZmSecurityModule>(m_db.get());
    m_audit = std::make_unique<ZmAuditModule>(m_db.get());
    // 门禁(横切;先装再注册业务接口)
    m_gate = std::make_unique<ZmAuthGateModule>(m_session.get(), m_permission.get());
    // 编排模块
    m_auth = std::make_unique<ZmAuthModule>(m_restful, m_user.get(), m_password.get(),
                                            m_session.get(), m_security.get(),
                                            m_audit.get(), m_permission.get(), m_db.get(), m_gate.get());
    m_admin = std::make_unique<ZmUserAdminModule>(m_restful, m_user.get(), m_password.get(),
                                                  m_session.get(), m_permission.get(),
                                                  m_security.get(), m_audit.get(), m_db.get(), m_gate.get());
    m_portal = std::make_unique<ZmPortalModule>(m_restful, m_user.get(), m_session.get(),
                                                m_permission.get(), m_gate.get());
}

// ============================================================================
// 前端页面路由(80/443)
// 页面组织与鉴权跳转规则见《2026-09-05-用户系统业务需求.md》§2.6/§3.1-3.3:
//   SPA 单一 index.html 由前端路由承载(/login /register /reset /force-reset /404
//   与 /portal 及子路径);页面渲染 + 302 跳转 + 白名单全部由门禁 advice 处理
//   (module_gate),本方法不再注册页面 handler。
// ============================================================================
void ServicePortal::RegisterFrontendRoutes(ZmHttpFrontendServer* fe)
{
    if (!fe)
        return;
    // 鉴权门禁(前端面):白名单 / 302 / force_change 边界 / SPA 页面响应
    m_gate->SetupFrontendGate(fe);
}

// ============================================================================
// JSON-RPC(39440):协议校验与信封由平台面内建(zm_net_http_jsonrpc_server),
// 业务层只注册 method 处理器;ping 为平台内建,无需注册。
// 本期无 JRPC 业务接口(用户系统全部走 RESTful)。
// ============================================================================
void ServicePortal::RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc)
{
    (void)jrpc;
}

// ============================================================================
// RESTful(39441 /zimo/api):门禁 advice 先行,再按模块注册业务接口
// (注册与命名规范见《2026-09-05-用户系统模块设计.md》§5)。
// ============================================================================
void ServicePortal::RegisterRestfulRoutes(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;
    // 鉴权门禁(RESTful 面):会话校验 / force_change 边界 / 接口级权限(横切)
    m_gate->SetupRestfulGate(rest);
    // 业务模块接口
    m_auth->RegisterRoutes();
    m_admin->RegisterRoutes();
    m_portal->RegisterRoutes();
}

// ============================================================================
// RESTful CORS(39441;预检 + 响应附加头,业务层显式,FR-21;设计 §11.4)
//   放行规则:CORS 白名单(Options.corsAllowedOrigins)命中,或"同站跨端口"
//   (Origin 与请求 Host 同 host,仅端口不同——页面 80/443 与 API 39441 场景)。
// ============================================================================
namespace
{
/// 同站判断:Origin(scheme://host[:port])与 Host 头(host[:port])去端口后一致
bool IsSameSiteHost(const string& origin, const string& hostHeader)
{
    size_t p = origin.find("://");
    if (p == string::npos)
        return false;
    string o = origin.substr(p + 3);
    auto stripPort = [](string h) -> string {
        if (!h.empty() && h.front() == '[')
        {   // IPv6 字面量
            size_t e = h.find(']');
            return e == string::npos ? h : h.substr(0, e + 1);
        }
        size_t c = h.rfind(':');
        return c == string::npos ? h : h.substr(0, c);
    };
    return stripPort(o) == stripPort(hostHeader);
}
}  // namespace

void ServicePortal::RegisterRestfulCors(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // CORS 白名单(Options.corsAllowedOrigins)+ 同站跨端口自动放行;
    // 均未命中 → 预检 403、响应不回显 CORS 头,浏览器判跨域失败。
    // OPTIONS 预检(FR-21)
    rest->RegisterPreRouting([](const HttpRequestPtr& req, AdviceCallback&& cb,
                                AdviceChainCallback&& cc) {
        if (req->method() != Options)
        {
            cc();
            return;
        }
        string origin = req->getHeader("Origin");
        if (!ZmHttpServer::IsCorsOriginAllowed(origin) &&
            !IsSameSiteHost(origin, req->getHeader("Host")))
        {
            // 白名单外且非同站:拒绝预检,不带任何 CORS 头(浏览器侧表现为跨域失败)
            cb(ZmHttpServer::ErrorResponse(403, "origin not allowed"));
            return;
        }
        ZMJSON d;
        auto resp = ZmHttpServer::JsonResponse(200, d);
        resp->addHeader("Access-Control-Allow-Origin", origin);
        resp->addHeader("Access-Control-Allow-Credentials", "true");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS, PATCH, DELETE");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Origin, Content-Type, Accept, X-File-Size");
        cb(resp);
    });

    // 响应附加 CORS 头(白名单命中或同站跨端口才回显 Origin + 凭据)
    rest->RegisterPreSending([](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
        if (req->method() == Options)
            return;   // 预检响应已带
        string origin = req->getHeader("Origin");
        if (!ZmHttpServer::IsCorsOriginAllowed(origin) &&
            !IsSameSiteHost(origin, req->getHeader("Host")))
            return;
        if (req->getHeader("cookie").find("zm_session") != string::npos ||
            resp->getStatusCode() >= k200OK)
        {
            resp->addHeader("Access-Control-Allow-Origin", origin);
            resp->addHeader("Access-Control-Allow-Credentials", "true");
        }
    });
}
