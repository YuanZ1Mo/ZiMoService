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
#include "modules/module_server_audio_stream.h"

// 文件中心
#include "modules/module_file_db.h"
#include "modules/module_file_store.h"
#include "modules/module_file_audit.h"
#include "modules/module_file_node.h"
#include "modules/module_file_admin.h"
#include "modules/module_file_task.h"
#include "modules/module_file_upload.h"
#include "modules/module_file_pack.h"
#include "modules/module_file_token.h"
#include "modules/module_file_share.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_admin.h"
#include "modules/module_file_hub.h"
#include "modules/util/dir_lock.h"

using namespace drogon;
using std::string;

namespace
{
/// CORS 预检实现(定义在文件后部:OPTIONS 命中放行规则 → 200 + 允许头)
void CorsPreflight(const HttpRequestPtr& req, AdviceCallback&& cb, AdviceChainCallback&& cc);
/// CORS 响应头回显实现(定义在文件后部)
void CorsEchoHeaders(const HttpRequestPtr& req, const HttpResponsePtr& resp);
}  // namespace

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================

/// 构造:仅保存宿主指针,服务器引用与业务模块在 Init() 内建立
ServicePortal::ServicePortal(NetDock* netDock)
    : m_netDock(netDock)
{
}

ServicePortal::~ServicePortal() = default;

/**
 * @brief 装配业务模块并注册全部路由与横切 advice(Phase1,须先于 ZmHttpServer::Open)
 *
 * 顺序:取三面实例 → 创建业务模块(建库建表与种子)→ 前端面路由/门禁 →
 * JRPC → RESTful 门禁与业务接口 → CORS。任一步失败只记日志,后续步骤继续。
 */
void ServicePortal::Init()
{
    if (!m_netDock)
    {
        DEFAULT_LOG_ERROR("ServicePortal::Init: NetDock 为空");
        return;
    }

    // 从宿主取三面实例(业务层只通过这些引用注册路由,不接触底层框架 API)
    m_frontend = m_netDock->GetFrontendServer();
    m_jrpc = m_netDock->GetJsonRpcServer();
    m_restful = m_netDock->GetRestfulServer();

    // 先创建业务模块(数据库建库建表种子;依赖注入),后续路由注册依赖这些模块
    CreateModules();

    // 按面注册:前端(页面/门禁)→ JRPC → RESTful(门禁 + 业务接口)→ CORS
    DEFAULT_LOG_INFO("Portal::Init 前端");
    RegisterFrontendRoutes(m_frontend);
    DEFAULT_LOG_INFO("Portal::Init JRPC");
    RegisterJsonRpcRoutes(m_jrpc);
    DEFAULT_LOG_INFO("Portal::Init REST");
    RegisterRestfulRoutes(m_restful);
    DEFAULT_LOG_INFO("Portal::Init CORS");    // 预检 + CORS 响应头挂在 RESTful 面(业务层显式)
    RegisterRestfulCors(m_restful);
    DEFAULT_LOG_INFO("Portal::Init 完成");
}

/// 业务收尾:本期无业务线程可 join;真正的关停在 ServiceCenter::OnStop 里
/// 先调本函数、再调 NetDock::Close(的"业务线程先收尾")
void ServicePortal::Shutdown()
{
    // 先收业务后台线程:服务器音频的采集线程持有系统音频设备,必须在网络层关闭前停下
    if (m_audio)
        m_audio->Shutdown();
}

/**
 * @brief 向广播端口发送消息
 *
 * 广播服务(39640 自定义 TCP)尚未接入,固定返回 false;签名保留以便业务侧调用点稳定。
 *
 * @param topic   主题
 * @param content 内容
 * @param tag     标签
 * @return 恒为 false
 */
bool ServicePortal::BroadcastMessage(const string& topic, const string& content,
                                     const string& tag)
{
    (void)topic; (void)content; (void)tag;
    DEFAULT_LOG_INFO("BroadcastMessage: 广播服务(39640)本期未接入");
    return false;
}

// ============================================================================
// 业务模块装配(依赖方向:编排模块 → 数据服务模块 → DbModule;门禁先装,以便业务
// 接口注册时即可挂门禁 advice)
// ============================================================================
/**
 * @brief 创建数据库模块与全部业务模块并完成依赖注入
 *
 * 数据库初始化失败时只记日志:其余模块仍会创建,相关接口在运行期返回内部错误。
 */
void ServicePortal::CreateModules()
{
    // 数据访问(全新建库建表 + 种子;库文件 exe 同级 db\user\user.db)
    m_db = std::make_unique<ZmDbModule>();
    if (!m_db->Init(ZmExeDir() + "db\\user\\user.db"))
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
    // 服务器音频(无独立数据模块:采集按需启停,分片只留内存)
    m_audio = std::make_unique<ZmServerAudioStreamModule>(m_restful, m_session.get(),
                                                          m_permission.get(), m_gate.get());
    m_audio->RegisterPermissions();

    // ── 文件中心:独立库 filehub.db + 物理根 modules\\filehub ──
    const std::string fileHubRoot = ZmExeDir() + "modules\\filehub";
    m_fileDb = std::make_unique<ZmFileDbModule>();
    if (!m_fileDb->Init(ZmExeDir() + "db\\filehub\\filehub.db", fileHubRoot))
    {
        DEFAULT_LOG_ERROR("ServicePortal::CreateModules: ZmFileDbModule::Init 失败,文件中心不可用");
    }
    else
    {
        m_fileDb->StartPeriodicCleanup();
    }
    m_fileStore = std::make_unique<ZmFileStoreModule>(m_fileDb.get(), fileHubRoot);
    m_fileAudit = std::make_unique<ZmFileAuditModule>(m_fileDb.get());
    m_dirLock = std::make_unique<ZmDirLock>();
    m_fileTask = std::make_unique<ZmFileTaskModule>(m_fileDb.get());
    m_fileNode = std::make_unique<ZmFileNodeModule>(m_fileDb.get(), m_fileStore.get(),
                                                    m_dirLock.get(), m_fileAudit.get());
    m_fileUpload = std::make_unique<ZmFileUploadModule>(m_fileDb.get(), m_fileStore.get(),
                                                        m_fileNode.get(), m_fileTask.get(),
                                                        m_fileAudit.get(), m_dirLock.get());
    m_fileToken = std::make_unique<ZmFileTokenModule>(m_fileStore.get(), m_fileNode.get(),
                                                      m_fileTask.get());
    m_filePack = std::make_unique<ZmFilePackModule>(m_fileDb.get(), m_fileStore.get(),
                                                    m_fileNode.get(), m_fileTask.get(),
                                                    m_fileAudit.get(), m_fileToken.get());
    m_fileShare = std::make_unique<ZmFileShareModule>(m_fileDb.get(), m_fileNode.get(),
                                                      m_fileStore.get(), m_fileAudit.get(),
                                                      m_fileToken.get(), m_filePack.get());
    m_fileHub = std::make_unique<ZmFileHubModule>(
        m_restful, m_session.get(), m_permission.get(), m_gate.get(), m_user.get(),
        m_fileDb.get(), m_fileNode.get(), m_fileTask.get(), m_fileAudit.get(),
        m_fileUpload.get(), m_filePack.get(), m_fileToken.get(), m_fileShare.get());
    m_fileAdmin = std::make_unique<ZmFileAdminModule>(
        m_restful, m_session.get(), m_permission.get(), m_gate.get(), m_user.get(),
        m_fileDb.get(), m_fileNode.get(), m_fileTask.get(), m_fileAudit.get(),
        m_filePack.get(), m_fileStore.get(), m_fileUpload.get());
    // 启动期收尾:僵尸任务置已中断 + 打包任务中断标记 + 历史回收站条目文件补搬
    m_fileTask->MarkZombieTasks();
    m_filePack->RecoverOrphans();
    m_fileNode->MigrateTrashLayout();
    m_fileAdmin->StartMaintenance();
    m_fileHub->StartDownloadSweeper();
    // 权限点登记(filehub / filehubAdmin;幂等)
    m_fileHub->RegisterPermissions();
    // 周期清理的物理侧动作(依赖倒置:底层只挑数据,物理删除由上层注入)
    ZmFileDbModule::CleanupHooks hooks;
    hooks.purgeExpiredTrash = [this](int64_t now) {
        if (m_fileNode)
            m_fileNode->PurgeExpiredSync(now - zm_file::kTrashRetainDays * 86400);
    };
    hooks.purgeUploadChunks = [this](int64_t now) {
        if (m_fileUpload)
            m_fileUpload->PurgeExpired(now);
    };
    hooks.cleanCache = [this](int64_t now) {
        if (m_filePack)
            m_filePack->CleanCache(now);
    };
    // 每日一致性校验默认开启(与其它清理项同为 03:00;可用配置关闭)
    // 走非协程入口:本钩子在工作池线程执行,协程版的返回 Task 无人 await 不会启动
    hooks.verifyConsistency = [this](int64_t now) {
        (void)now;
        if (m_fileAdmin && !m_fileAdmin->StartSyncDetached(false, ZmOpCtx{0, "system", ""}))
            DEFAULT_LOG_WARN("每日一致性校验未启动(已有一轮在运行或任务行创建失败)");
    };
    m_fileDb->SetCleanupHooks(hooks);
}

// ============================================================================
// 前端页面路由(80/443)
// 页面组织与鉴权跳转规则见《2026-09-05-用户系统业务需求.md》:
//   SPA 单一 index.html 由前端路由承载(/login /register /reset /force-reset /404
//   与 /portal 及子路径);页面渲染 + 302 跳转 + 白名单全部由门禁 advice 处理
//   (module_gate),故本方法不注册页面 handler。
// ============================================================================
/**
 * @brief 注册前端面(80/443)的页面门禁
 *
 * 页面响应与跳转全部由门禁 advice 承担,此处只做挂载。
 *
 * @param fe 前端面实例(primary);为空则跳过
 */
void ServicePortal::RegisterFrontendRoutes(ZmHttpFrontendServer* fe)
{
    if (!fe)
        return;
    m_gate->SetupFrontendGate(fe);
}

/**
 * @brief 注册 JSON-RPC 面的业务 method
 *
 * 协议校验与信封由平台面内建,业务层只需注册 method 处理器;当前仅注册健康检查 ping。
 *
 * @param jrpc JSON-RPC 面实例;为空则跳过
 */
void ServicePortal::RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc)
{
    if (!jrpc)
        return;
    // 健康检查:ping → result {"pong":true}(与 RESTful /ping 语义一致)
    jrpc->RegisterMethod("ping", [](const ZMJSON& params, ZMJSON& result, ZMJSON& error) -> bool {
        (void)params;
        (void)error;
        result["pong"] = true;
        return true;
    });
}

/**
 * @brief 注册 RESTful 面门禁与各业务模块接口
 *
 * 顺序有讲究:门禁 advice 必须**先注册**,使该面的鉴权裁决先于业务 handler 注册的
 * 其他 advice 生效(接口注册与命名规范见《2026-09-05-用户系统模块设计.md》)。
 *
 * @param rest RESTful 面实例;为空则跳过
 */
void ServicePortal::RegisterRestfulRoutes(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // 会话/强制改密/接口级权限的横切门禁
    m_gate->SetupRestfulGate(rest);

    // 各业务模块的接口(模块内部逐条注册路径与 handler)
    m_auth->RegisterRoutes();
    m_admin->RegisterRoutes();
    m_portal->RegisterRoutes();
    m_fileHub->RegisterRoutes();
    m_fileAdmin->RegisterRoutes();
    m_audio->RegisterRoutes();
}

// ============================================================================
// RESTful CORS(39441;预检 + 响应附加头,业务层显式)
//   放行规则:CORS 白名单(SetCorsAllowedOrigins 声明)命中,或"同站跨端口"
//   (Origin 与请求 Host 同 host,仅端口不同——页面 80/443 与 API 39441 场景)。
// ============================================================================
namespace
{
/**
 * @brief 去掉主机串里的端口段(IPv6 字面量按 ']' 之后判定)
 *
 * @param h host[:port],如 "[::1]:80"、"a:80"、"a"
 * @return 去端口后的主机串
 */
string StripHostPort(string h)
{
    if (!h.empty() && h.front() == '[')
    {   // IPv6 字面量:端口分隔符在 ']' 之后
        size_t e = h.find(']');
        return e == string::npos ? h : h.substr(0, e + 1);
    }
    size_t c = h.rfind(':');
    return c == string::npos ? h : h.substr(0, c);
}

/**
 * @brief 判断 Origin 与请求 Host 是否"同站"(忽略端口)
 *
 * 用于放行"同站跨端口"场景:页面在 80/443、API 在 39441,浏览器视其同站但会发
 * CORS 预检,这种请求不经过白名单直接放行。
 *
 * @param origin     Origin 头(scheme://host[:port])
 * @param hostHeader Host 头(host[:port])
 * @return true 两者去端口后 host 相同;false 非同站或 Origin 格式不合法
 */
bool IsSameSiteHost(const string& origin, const string& hostHeader)
{
    size_t p = origin.find("://");
    if (p == string::npos)
        return false;

    // 取 scheme:// 之后的 host[:port] 部分再比对
    string o = origin.substr(p + 3);
    return StripHostPort(o) == StripHostPort(hostHeader);
}
}  // namespace

/**
 * @brief 注册 CORS 预检与响应头回显(仅 RESTful 面)
 *
 * 放行条件(两条满足其一):Origin 命中白名单(SetCorsAllowedOrigins 声明),或
 * "同站跨端口"(Origin 与请求 Host 同 host,仅端口不同,如页面 443 → API 39441)。
 * 两者都不满足 → 预检 403,且普通响应不回显任何 CORS 头(浏览器按跨域失败处理)。
 *
 * @param rest RESTful 面实例;为空则跳过
 */
void ServicePortal::RegisterRestfulCors(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // ── OPTIONS 预检:命中放行规则则 200 + 允许头,否则 403 ──
    rest->RegisterPreRouting([](const HttpRequestPtr& req, AdviceCallback&& cb,
                                AdviceChainCallback&& cc) {
        CorsPreflight(req, std::move(cb), std::move(cc));
    });

    // ── 普通响应:同样按放行规则决定是否回显 CORS 头(预检响应已带,跳过) ──
    rest->RegisterPreSending([](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
        CorsEchoHeaders(req, resp);
    });
}

namespace
{
/**
 * @brief CORS 预检:OPTIONS 命中放行规则 → 200 + 允许头,否则 403
 *
 * @param req 请求(读 Origin 与 Host)
 * @param cb  短路回调
 * @param cc  放行回调(非 OPTIONS 请求)
 */
void CorsPreflight(const HttpRequestPtr& req, AdviceCallback&& cb, AdviceChainCallback&& cc)
{
    if (req->method() != Options)
    {
        cc();   // 非预检请求交给后续处理
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

    // 命中:回显 Origin 与凭据头,并声明允许的方法与请求头
    // 方法与请求头 = 业务实际使用面:上传走 PUT(分片),元数据走自定义头
    // (X-Space/X-Dir-Id/X-File-Name/X-Conflict/X-Upload-Id/X-Chunk-Index),
    // 漏一个浏览器就会在预检处拦掉整个上传 —— 新增自定义头时必须同步本清单
    ZMJSON d;
    auto resp = ZmHttpServer::JsonResponse(200, d);
    resp->addHeader("Access-Control-Allow-Origin", origin);
    resp->addHeader("Access-Control-Allow-Credentials", "true");
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS, PATCH, DELETE");
    resp->addHeader("Access-Control-Allow-Headers",
                    "Origin, Content-Type, Accept, X-File-Size, X-Space, X-Dir-Id, "
                    "X-File-Name, X-Conflict, X-Upload-Id, X-Chunk-Index");
    cb(resp);
}

/**
 * @brief CORS 响应头回显:命中放行规则且(带会话或成功响应)才回显
 *
 * @param req  请求
 * @param resp 即将发送的响应
 */
void CorsEchoHeaders(const HttpRequestPtr& req, const HttpResponsePtr& resp)
{
    if (req->method() == Options)
        return;

    string origin = req->getHeader("Origin");
    if (!ZmHttpServer::IsCorsOriginAllowed(origin) &&
        !IsSameSiteHost(origin, req->getHeader("Host")))
        return;

    // 带会话的响应与成功响应才回显(凭据头与实时状态码一起下发,避免缓存串味)
    if (req->getHeader("cookie").find("zm_session") != string::npos ||
        resp->getStatusCode() >= k200OK)
    {
        resp->addHeader("Access-Control-Allow-Origin", origin);
        resp->addHeader("Access-Control-Allow-Credentials", "true");
    }
}
}  // namespace
