#include "modules/module_file_hub.h"

#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_pack.h"
#include "modules/module_file_share.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"
#include "modules/module_file_token.h"
#include "modules/module_file_upload.h"
#include "modules/module_gate.h"
#include "modules/module_permission.h"
#include "modules/module_session.h"
#include "modules/module_user.h"

#include <drogon/HttpAppFramework.h>

#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <memory>
#include <set>

using namespace drogon;

namespace
{
/// @brief 取整型查询参数(越界时钳到边界)
int QueryInt(const HttpRequestPtr& req, const char* name, int def, int minV, int maxV)
{
    std::string s = req->getParameter(name);
    if (s.empty())
        return def;
    try
    {
        int v = std::stoi(s);
        return std::min(maxV, std::max(minV, v));
    }
    catch (...)
    {
        return def;
    }
}

/// @brief 取 64 位整型查询参数(缺省 0)
int64_t QueryI64(const HttpRequestPtr& req, const char* name, int64_t def)
{
    std::string s = req->getParameter(name);
    if (s.empty())
        return def;
    try
    {
        return std::stoll(s);
    }
    catch (...)
    {
        return def;
    }
}

/// @brief 取字符串查询参数
std::string QueryStr(const HttpRequestPtr& req, const char* name)
{
    return req->getParameter(name);
}

/// @brief 解析请求体 JSON(非法/为空返回空对象)
ZMJSON ParseBody(const HttpRequestPtr& req)
{
    std::string_view sv = req->getBody();
    if (sv.empty())
        return ZMJSON::object();
    std::string err;
    ZMJSON      j = zm_json_parse(std::string(sv), err);
    // 解析失败返回空对象;调用方按"字段缺失"处理
    return j.is_object() ? j : ZMJSON::object();
}

/// @brief 解析 id 数组(去重、丢弃非正数;key 默认 "ids")
std::vector<int64_t> BodyIds(const ZMJSON& body, const char* key = "ids")
{
    std::vector<int64_t> ids;
    if (!body.contains(key) || !body[key].is_array())
        return ids;
    std::set<int64_t> seen;
    for (const auto& v : body[key])
    {
        int64_t id = 0;
        if (v.is_number_integer())
            id = v.get<int64_t>();
        else if (v.is_string())
        {
            try
            {
                id = std::stoll(v.get<std::string>());
            }
            catch (...)
            {
                id = 0;
            }
        }
        if (id > 0 && !seen.count(id))
        {
            seen.insert(id);
            ids.push_back(id);
        }
    }
    return ids;
}

/// @brief 解析字符串数组
std::vector<std::string> BodyStrings(const ZMJSON& body, const char* key)
{
    std::vector<std::string> out;
    if (!body.contains(key) || !body[key].is_array())
        return out;
    for (const auto& v : body[key])
    {
        if (v.is_string())
            out.push_back(v.get<std::string>());
    }
    return out;
}

/// @brief 由查询参数构造列表查询
ZmListQuery ListQueryOf(const HttpRequestPtr& req, int defSize)
{
    ZmListQuery q;
    q.sort = QueryStr(req, "sort");
    if (q.sort.empty())
        q.sort = "name";
    q.order = QueryStr(req, "order");
    if (q.order.empty())
        q.order = "asc";
    q.page       = QueryInt(req, "page", 1, 1, 1000000);
    q.size       = QueryInt(req, "size", defSize, 1, 1000);
    q.typeFilter = QueryStr(req, "type");
    q.mtimeFrom  = QueryI64(req, "mtime_from", 0);
    q.mtimeTo    = QueryI64(req, "mtime_to", 0);
    return q;
}

/// @brief 站点基址(下载直链用:REST 端口)
std::string SiteBase(const HttpRequestPtr& req, ZmHttpRestfulServer* rest)
{
    std::string host  = req->getHeader("Host");
    size_t      colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos)
        host = host.substr(0, colon);
    std::string scheme = (rest && rest->IsHttps()) ? "https" : "http";
    uint16_t    port   = rest ? rest->GetPort() : 39441;
    return scheme + "://" + host + ":" + std::to_string(port);
}

/// @brief 站点页面基址(分享链接用:页面端口 80/443,标准端口在 URL 中省略)
///
/// 分享链接是给人复制粘贴、印二维码的,必须落在**页面端口**上(需求 §3.12.1);
/// 用 REST 端口(39441)拼出来的链接浏览器直接 404 —— 页面服务只在 80/443 监听。
/// 页面走标准端口故此处不带端口号,与下载直链的 SiteBase(REST 端口)刻意分开:
/// 直链要 39441,分享链接要 80/443,两者不是同一个基址。
std::string SitePageBase(const HttpRequestPtr& req, ZmHttpRestfulServer* rest)
{
    std::string host  = req->getHeader("Host");
    size_t      colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos)
        host = host.substr(0, colon);
    std::string scheme = (rest && rest->IsHttps()) ? "https" : "http";
    return scheme + "://" + host;
}
} // namespace

// ============================================================================
// 构造 / 析构 / 注册
// ============================================================================
ZmFileHubModule::ZmFileHubModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                                 ZmPermissionModule* permission, ZmAuthGateModule* gate,
                                 ZmUserModule* user, ZmFileDbModule* db,
                                 ZmFileNodeModule* node, ZmFileTaskModule* task,
                                 ZmFileAuditModule* audit, ZmFileUploadModule* upload,
                                 ZmFilePackModule* pack, ZmFileTokenModule* token,
                                 ZmFileShareModule* share)
    : m_rest(rest), m_session(session), m_permission(permission), m_gate(gate), m_user(user),
      m_db(db), m_node(node), m_task(task), m_audit(audit), m_upload(upload), m_pack(pack),
      m_token(token), m_share(share)
{
}

ZmFileHubModule::~ZmFileHubModule()
{
    // 重试执行体捕获了本模块,先把登记撤掉再析构,避免悬挂回调
    if (m_task)
    {
        m_task->RegisterRetryExec(zm_file::kTaskCopy, nullptr);
        m_task->RegisterRetryExec(zm_file::kTaskPack, nullptr);
        m_task->RegisterRetryExec(zm_file::kTaskStat, nullptr);
        m_task->RegisterRetryExec(zm_file::kTaskTrashClear, nullptr);
        m_task->RegisterRetryExec(zm_file::kTaskUpload, nullptr);
    }
}

// ============================================================================
// 门禁与响应
// ============================================================================
drogon::Task<ZmGateResult> ZmFileHubModule::Authorize(const HttpRequestPtr& req, bool admin)
{
    ZmGateResult      r;
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    r.ctx  = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto g = ZmAuthGateModule::CheckCtxSync(r.ctx, req->path());
    if (!g.ok)
    {
        r.status  = g.status;
        r.code    = g.code;
        r.message = g.message;
        co_return r;
    }
    // 文件中心不设独立功能权限点:门户点 filehub 同时充当 API 门禁点
    if (!(co_await m_permission->HasPermission(r.ctx.uid, admin ? "filehubAdmin" : "filehub")))
    {
        r.status  = 403;
        r.code    = "PERM_DENIED";
        r.message = "无权限访问";
        co_return r;
    }
    r.ok = true;
    co_return r;
}

HttpResponsePtr ZmFileHubModule::Respond(const ZMJSON& out)
{
    if (!ZmFileHasError(out))
        return ZmAuthGateModule::ApiOk(out);
    const ZMJSON& e       = out["error"];
    int           status  = ZmFileErrorStatus(out);
    std::string   code    = ZmFileErrorCode(out);
    std::string   message = ZmFileErrorMessage(out);
    // 附加字段(如冲突清单)平铺到响应体顶层:前端从 error.data.<字段> 读取
    ZMJSON body     = ZMJSON::object();
    body["code"]    = code;
    body["message"] = message;
    for (auto it = e.begin(); it != e.end(); ++it)
    {
        const std::string& k = it.key();
        if (k == "code" || k == "message" || k == "status")
            continue;
        body[k] = it.value();
    }
    return ZmHttpServer::JsonResponse(status, body);
}

ZmOpCtx ZmFileHubModule::OpOf(const ZmSessionCtx& ctx, const HttpRequestPtr& req)
{
    ZmOpCtx op;
    op.uid     = ctx.uid;
    op.account = ctx.account;
    op.ip      = ZmAuthGateModule::ClientIp(req);
    return op;
}

// ============================================================================
// 归属名解析
// ============================================================================
drogon::Task<void> ZmFileHubModule::FillOwnerNames(ZMJSON& list)
{
    if (!list.is_array() || list.empty())
        co_return;
    std::vector<int64_t> uids;
    for (const auto& row : list)
    {
        int64_t uid = zm_file_row_int(row, "owner_uid", 0);
        if (uid > 0)
            uids.push_back(uid);
    }
    ZMJSON names = co_await m_user->GetNicknames(uids);
    for (auto& row : list)
    {
        int64_t uid = zm_file_row_int(row, "owner_uid", 0);
        if (uid == 0)
            row["owner_name"] = "系统";
        else
        {
            std::string key = std::to_string(uid);
            row["owner_name"] =
                names.contains(key) ? names[key].get<std::string>() : ("用户" + key);
        }
    }
    co_return;
}

drogon::Task<void> ZmFileHubModule::FillTrashNames(ZMJSON& list)
{
    if (!list.is_array() || list.empty())
        co_return;
    std::vector<int64_t> uids;
    for (const auto& row : list)
    {
        int64_t uid = zm_file_row_int(row, "del_owner_uid", 0);
        if (uid > 0)
            uids.push_back(uid);
        if (row.contains("space"))
        {
            int64_t sp = zm_file_row_int(row, "space", 0);
            if (sp > 0)
                uids.push_back(sp);
        }
    }
    ZMJSON names = co_await m_user->GetNicknames(uids);
    for (auto& row : list)
    {
        int64_t uid = zm_file_row_int(row, "del_owner_uid", 0);
        if (uid == 0)
            row["del_owner_name"] = "系统";
        else
        {
            std::string key = std::to_string(uid);
            row["del_owner_name"] =
                names.contains(key) ? names[key].get<std::string>() : ("用户" + key);
        }
        if (row.contains("space"))
        {
            int64_t sp = zm_file_row_int(row, "space", 0);
            if (sp == 0)
                row["space_name"] = "公共空间";
            else
            {
                std::string key = std::to_string(sp);
                row["space_name"] =
                    names.contains(key) ? names[key].get<std::string>() : ("用户 " + key);
            }
        }
    }
    co_return;
}

// ============================================================================
// 路由注册
// ============================================================================
void ZmFileHubModule::RegisterRoutes()
{
    if (!m_rest)
        return;
    const char* k = "/zimo/api/filehub";
    // 空间与浏览
    m_rest->RegisterCoro(std::string(k) + "/spaces", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSpaces(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/list", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleList(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/search", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSearch(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/node/{1}", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleNode(std::move(req), std::move(idStr)); });
    m_rest->RegisterCoro(std::string(k) + "/stat", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleStat(std::move(req)); });
    // 条目操作
    m_rest->RegisterCoro(std::string(k) + "/dirs", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleMkdir(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/dirs/ensure", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleEnsure(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/dirs/ensure_batch", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleEnsureBatch(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/nodes/{1}", HttpMethod::Patch,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleRename(std::move(req), std::move(idStr)); });
    m_rest->RegisterCoro(std::string(k) + "/nodes/move", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleMove(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/nodes/copy", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleCopy(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/nodes/delete", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleDelete(std::move(req)); });
    // 回收站
    m_rest->RegisterCoro(std::string(k) + "/trash", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrash(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/restore", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashRestore(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/purge", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashPurge(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/clear", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashClear(std::move(req)); });
    // 上传
    m_rest->RegisterCoro(std::string(k) + "/upload/simple", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleUploadSimple(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/upload/init", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleUploadInit(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/upload/chunk", HttpMethod::Put,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleUploadChunk(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/upload/complete", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleUploadComplete(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/upload/{1}", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string uploadId) -> Task<HttpResponsePtr>
        { return HandleUploadStatus(std::move(req), std::move(uploadId)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/upload/{1}", HttpMethod::Delete,
        [this](HttpRequestPtr req, std::string uploadId) -> Task<HttpResponsePtr>
        { return HandleUploadCancel(std::move(req), std::move(uploadId)); });
    // 下载与打包
    m_rest->RegisterCoro(std::string(k) + "/download/token", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleDownloadToken(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/dl/{1}/{2}", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string token,
               std::string filename) -> Task<HttpResponsePtr>
        { return HandleDownload(std::move(req), std::move(token), std::move(filename)); });
    m_rest->RegisterCoro(std::string(k) + "/pack", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandlePack(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/pack/{1}/clean", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandlePackClean(std::move(req), std::move(taskNo)); });
    // 传输任务(字面量路由先注册,避免被 /tasks/{1} 抢匹配)
    m_rest->RegisterCoro(std::string(k) + "/tasks/active", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTasksActive(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/tasks/clear", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTasksClear(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/tasks", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTasks(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/tasks/{1}", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandleTaskDetail(std::move(req), std::move(taskNo)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/tasks/{1}/cancel", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandleTaskCancel(std::move(req), std::move(taskNo)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/tasks/{1}/retry", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandleTaskRetry(std::move(req), std::move(taskNo)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/tasks/{1}", HttpMethod::Delete,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandleTaskDelete(std::move(req), std::move(taskNo)); });
    // 分享(登录侧)
    m_rest->RegisterCoro(std::string(k) + "/shares", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleShareCreate(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/shares", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleShareList(std::move(req)); });
    // 字面量路由先注册,避免被 /shares/{1} 形态抢匹配
    m_rest->RegisterCoro(std::string(k) + "/shares/purge", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSharePurge(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/shares/{1}", HttpMethod::Patch,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleSharePatch(std::move(req), std::move(idStr)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/shares/{1}", HttpMethod::Delete,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleShareCancel(std::move(req), std::move(idStr)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/shares/{1}/resume", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleShareResume(std::move(req), std::move(idStr)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/shares/{1}/logs", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string idStr) -> Task<HttpResponsePtr>
        { return HandleShareLogs(std::move(req), std::move(idStr)); });
    // 分享(公开面,免会话;门禁白名单已放行)
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/share/{1}", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string token) -> Task<HttpResponsePtr>
        { return HandleShareInfo(std::move(req), std::move(token)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/share/{1}/verify", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string token) -> Task<HttpResponsePtr>
        { return HandleShareVerify(std::move(req), std::move(token)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/share/{1}/list", HttpMethod::Get,
        [this](HttpRequestPtr req, std::string token) -> Task<HttpResponsePtr>
        { return HandleShareListDir(std::move(req), std::move(token)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/share/{1}/download", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string token) -> Task<HttpResponsePtr>
        { return HandleShareDownload(std::move(req), std::move(token)); });
    RegisterRetryExecs();
    DEFAULT_LOG_INFO("ZmFileHubModule: /filehub/* 接口已注册");
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::DenyForeignSpace(int64_t space,
                                                               const ZmOpCtx& ctx,
                                                               const std::string& action)
{
    if (ZmFileNodeModule::SpaceWritable(space, ctx.uid))
        co_return HttpResponsePtr{};
    // 被拒绝的请求也要留痕:记 result=2 + 动作码,便于排查"为什么删不掉"与越权尝试
    co_await ZmHttpServer::RunOnPool<bool>(
        [this, space, &ctx, &action]() -> bool
        {
            return m_db->WithTxSync(
                [&](ZmSqliteDb& db) -> bool
                {
                    return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, action, space, 0,
                                                     "", "{\"error\":\"" +
                                                             std::string(zm_file_err::kPermDenied) +
                                                             "\"}",
                                                     ctx.ip, 2);
                });
        });
    co_return ZmAuthGateModule::ApiError(403, zm_file_err::kPermDenied, "无权访问该空间");
}

void ZmFileHubModule::StartDownloadSweeper()
{
    if (!m_token)
        return;
    drogon::app().registerBeginningAdvice(
        [this]()
        {
            trantor::EventLoop* loop = drogon::app().getLoop();
            if (!loop)
                return;
            loop->runEvery(30.0, [this]() { m_token->SweepLeaks(ZmSqliteDb::Now()); });
            DEFAULT_LOG_INFO("ZmFileHubModule: 下载名额巡检已注册(每 30 秒)");
        });
}

void ZmFileHubModule::RegisterRetryExecs()
{
    if (!m_task)
        return;
    m_task->RegisterRetryExec(zm_file::kTaskPack,
                              [this](const ZMJSON& t) -> drogon::Task<ZMJSON>
                              { return m_pack->Retry(t); });
    m_task->RegisterRetryExec(zm_file::kTaskUpload,
                              [this](const ZMJSON& t) -> drogon::Task<ZMJSON>
                              { return m_upload->Retry(t); });
    m_task->RegisterRetryExec(
        zm_file::kTaskCopy,
        [this](const ZMJSON& t) -> drogon::Task<ZMJSON>
        {
            std::string taskNo  = zm_file_row_str(t, "task_no");
            ZMJSON      payload = m_task->GetRetryPayload(taskNo);
            if (payload.empty())
                co_return ZmFileError(zm_file_err::kBadRequest, 400,
                                      "复制任务的输入已失效,请重新发起");
            ZmOpCtx ctx;
            ctx.uid                        = zm_file_row_int(payload, "uid", 0);
            ctx.account                    = zm_file_row_str(payload, "account");
            ctx.ip                         = zm_file_row_str(payload, "ip");
            int64_t              space     = zm_file_row_int(payload, "space", 0);
            int64_t              targetDir = zm_file_row_int(payload, "target_dir", 0);
            std::string          conflict  = zm_file_row_str(payload, "conflict");
            std::vector<int64_t> ids;
            for (const auto& v : payload["ids"])
                ids.push_back(v.get<int64_t>());
            ZMJSON created = co_await m_task->Start(
                zm_file::kTaskCopy, ctx.uid, space, zm_file_row_str(t, "name"), "", 0, 0, "",
                [this, ids, space, targetDir, conflict, ctx](ZmTaskHandle& h)
                {
                    ZMJSON r = m_node->CopyExec(ids, space, targetDir, conflict, ctx, &h);
                    if (ZmFileHasError(r))
                        h.Finish(zm_file::kTaskFailed, ZmFileErrorMessage(r));
                    else
                        h.Finish(zm_file::kTaskDone, "", r.dump());
                });
            std::string newNo = zm_file_row_str(created, "task_no");
            if (!newNo.empty())
                m_task->SetRetryPayload(newNo, payload);
            co_return created;
        });
    m_task->RegisterRetryExec(
        zm_file::kTaskStat,
        [this](const ZMJSON& t) -> drogon::Task<ZMJSON>
        {
            std::string taskNo  = zm_file_row_str(t, "task_no");
            ZMJSON      payload = m_task->GetRetryPayload(taskNo);
            if (payload.empty() || !payload.contains("ids"))
                co_return ZmFileError(zm_file_err::kBadRequest, 400,
                                      "统计任务的输入已失效,请重新发起");
            std::vector<int64_t> ids;
            for (const auto& v : payload["ids"])
                ids.push_back(v.get<int64_t>());
            int64_t uid     = zm_file_row_int(t, "uid", 0);
            int64_t space   = zm_file_row_int(t, "space", 0);
            ZMJSON  created = co_await m_task->Start(
                zm_file::kTaskStat, uid, space, zm_file_row_str(t, "name"), "", 0, 0, "",
                [this, ids](ZmTaskHandle& h)
                {
                    ZMJSON r = m_node->StatExec(ids, &h);
                    if (ZmFileHasError(r))
                        h.Finish(zm_file::kTaskFailed, ZmFileErrorMessage(r));
                    else
                        h.Finish(zm_file::kTaskDone, "", r.dump());
                });
            co_return created;
        });
    m_task->RegisterRetryExec(
        zm_file::kTaskTrashClear,
        [this](const ZMJSON& t) -> drogon::Task<ZMJSON>
        {
            std::string taskNo  = zm_file_row_str(t, "task_no");
            ZMJSON      payload = m_task->GetRetryPayload(taskNo);
            if (payload.empty())
                co_return ZmFileError(zm_file_err::kBadRequest, 400,
                                      "清理任务的输入已失效,请重新发起");
            int64_t     uid     = zm_file_row_int(payload, "uid", 0);
            int64_t     space   = zm_file_row_int(payload, "space", 0);
            std::string account = zm_file_row_str(payload, "account");
            std::string ip      = zm_file_row_str(payload, "ip");
            ZMJSON      created = co_await m_task->Start(
                zm_file::kTaskTrashClear, uid, space, zm_file_row_str(t, "name"), "", 0, 0, "",
                [this, space, account, ip, uid](ZmTaskHandle& h)
                {
                    ZmOpCtx ctx;
                    ctx.uid     = uid;
                    ctx.account = account;
                    ctx.ip      = ip;
                    ZMJSON r    = m_node->ClearTrashExec(space, false, ctx, &h);
                    h.Finish(zm_file::kTaskDone, "", r.dump());
                });
            std::string newNo = zm_file_row_str(created, "task_no");
            if (!newNo.empty())
                m_task->SetRetryPayload(newNo, payload);
            co_return created;
        });
}

// ============================================================================
// 权限点登记(幂等;由装配层在用户系统种子之后调用)
// ============================================================================
void ZmFileHubModule::RegisterPermissions()
{
    if (!m_permission)
        return;
    // 权限点属于 user.db,但归属文件中心业务:经权限模块写入(幂等),不写进用户系统种子
    ZMJSON hub           = ZMJSON::object();
    hub["code"]          = "filehub";
    hub["name"]          = "文件中心";
    hub["module"]        = "filehub";
    hub["url"]           = "/portal/filehub";
    hub["type"]          = 0; // 门户模块:进侧边栏
    hub["index"]         = 2; // 排在用户主页(1)之后、系统管理(-1)之前
    hub["sort"]          = 4;
    hub["enabled"]       = 1;
    hub["description"]   = "文件中心门户模块";
    ZMJSON admin         = ZMJSON::object();
    admin["code"]        = "filehubAdmin";
    admin["name"]        = "文件中心管理";
    admin["module"]      = "system";
    admin["url"]         = "";
    admin["type"]        = 1; // 功能权限:不进侧边栏,只守管理端接口与子标签
    admin["index"]       = 0;
    admin["sort"]        = 5;
    admin["enabled"]     = 1;
    admin["description"] = "系统管理-文件中心管理功能权限";
    // 默认可见性:门户点对所有角色开放;管理点仅 developer/admin(与 userManage 一致)
    bool ok = m_permission->RegisterPermCodeSync(hub, {"developer", "admin", "user"});
    ok      = m_permission->RegisterPermCodeSync(admin, {"developer", "admin"}) && ok;
    if (!ok)
        DEFAULT_LOG_ERROR("ZmFileHubModule: 权限点登记失败(filehub / filehubAdmin)");
    else
        DEFAULT_LOG_INFO("ZmFileHubModule: 权限点已登记(filehub / filehubAdmin)");
}

// ============================================================================
// 空间与浏览
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleSpaces(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    // 个人空间懒创建:首次访问即建行 + 建物理目录(幂等)
    co_await m_db->EnsureSpace(gate.ctx.uid);
    ZMJSON rows = co_await m_db->ListSpaces();
    ZMJSON out  = ZMJSON::array();
    for (const auto& r : rows)
    {
        int64_t space = zm_file_row_int(r, "space", 0);
        // 只下发公共空间与"我自己的"个人空间:他人空间可见性是数据边界
        if (space != 0 && space != gate.ctx.uid)
            continue;
        ZMJSON item        = ZMJSON::object();
        item["space"]      = space;
        item["name"]       = space == 0 ? "公共空间" : "我的空间";
        item["quota"]      = zm_file_row_int(r, "quota", 0);
        item["used_size"]  = zm_file_row_int(r, "used_size", 0);
        item["used_items"] = zm_file_row_int(r, "used_items", 0);
        // 回收站占用按"我删的"口径(不等于空间级 used_size)
        int64_t tb = 0;
        int64_t ti = 0;
        ZmFileDbModule::TrashUsageSync(*m_db, space, gate.ctx.uid, tb, ti);
        item["my_trash_size"]  = tb;
        item["my_trash_items"] = ti;
        out.push_back(std::move(item));
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleList(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t space = QueryI64(req, "space", 0);
    if (!ZmFileNodeModule::SpaceWritable(space, gate.ctx.uid))
        co_return ZmAuthGateModule::ApiError(403, zm_file_err::kPermDenied, "无权访问该空间");
    int64_t     dirId = QueryI64(req, "dir_id", 0);
    ZmListQuery q     = ListQueryOf(req, 200);
    ZMJSON      out   = co_await m_node->List(space, dirId, q);
    if (!ZmFileHasError(out))
        co_await FillOwnerNames(out["list"]);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleSearch(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t space = QueryI64(req, "space", 0);
    if (!ZmFileNodeModule::SpaceWritable(space, gate.ctx.uid))
        co_return ZmAuthGateModule::ApiError(403, zm_file_err::kPermDenied, "无权访问该空间");
    int64_t     dirId   = QueryI64(req, "dir_id", 0);
    std::string keyword = QueryStr(req, "keyword");
    ZmListQuery q       = ListQueryOf(req, 200);
    q.sort              = QueryStr(req, "sort");
    q.order             = QueryStr(req, "order");
    ZMJSON out          = co_await m_node->Search(space, dirId, keyword, q);
    if (!ZmFileHasError(out))
        co_await FillOwnerNames(out["list"]);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleNode(HttpRequestPtr req,
                                                          std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "条目 id 非法");
    }
    ZMJSON out = co_await m_node->Detail(id);
    if (!ZmFileHasError(out))
    {
        ZMJSON list = ZMJSON::array();
        list.push_back(out);
        co_await FillOwnerNames(list);
        out = list[0];
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleStat(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (!ZmFileNodeModule::SpaceWritable(space, gate.ctx.uid))
        co_return ZmAuthGateModule::ApiError(403, zm_file_err::kPermDenied, "无权访问该空间");
    std::vector<int64_t> ids = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZmOpCtx ctx     = OpOf(gate.ctx, req);
    ZMJSON  payload = ZMJSON::object();
    payload["ids"]  = ids;
    ZMJSON created =
        co_await m_task->Start(zm_file::kTaskStat, gate.ctx.uid, space,
                               "统计 " + std::to_string(ids.size()) + " 项", "", 0, 0, "",
                               [this, ids](ZmTaskHandle& h)
                               {
                                   ZMJSON r = m_node->StatExec(ids, &h);
                                   if (ZmFileHasError(r))
                                       h.Finish(zm_file::kTaskFailed, ZmFileErrorMessage(r));
                                   else
                                       h.Finish(zm_file::kTaskDone, "", r.dump());
                               });
    std::string taskNo = zm_file_row_str(created, "task_no");
    if (!taskNo.empty())
        m_task->SetRetryPayload(taskNo, payload);
    (void)ctx;
    co_return Respond(created);
}

// ============================================================================
// 条目操作
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleMkdir(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (auto deny = co_await DenyForeignSpace(space, OpOf(gate.ctx, req), zm_file::kActMkdir))
        co_return deny;
    int64_t     parentId = zm_file_row_int(body, "parent_id", 0);
    std::string name     = zm_file_row_str(body, "name");
    if (name.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少目录名");
    ZMJSON out = co_await m_node->Mkdir(space, parentId, name, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleEnsure(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (auto deny = co_await DenyForeignSpace(space, OpOf(gate.ctx, req), zm_file::kActMkdir))
        co_return deny;
    int64_t     parentId = zm_file_row_int(body, "parent_id", 0);
    std::string path     = zm_file_row_str(body, "path");
    if (path.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少相对路径");
    ZMJSON out =
        co_await m_node->EnsureDirs(space, parentId, {path}, false, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleEnsureBatch(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (auto deny = co_await DenyForeignSpace(space, OpOf(gate.ctx, req), zm_file::kActMkdir))
        co_return deny;
    int64_t                  parentId = zm_file_row_int(body, "parent_id", 0);
    std::vector<std::string> paths    = BodyStrings(body, "paths");
    if (paths.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少 paths");
    ZMJSON out =
        co_await m_node->EnsureDirs(space, parentId, paths, true, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleRename(HttpRequestPtr req,
                                                            std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "条目 id 非法");
    }
    ZMJSON      body = ParseBody(req);
    std::string name = zm_file_row_str(body, "name");
    if (name.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少新名称");
    ZMJSON out = co_await m_node->Rename(id, name, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleMove(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    int64_t     targetSpace = zm_file_row_int(body, "target_space", 0);
    int64_t     targetDir   = zm_file_row_int(body, "target_dir_id", 0);
    std::string conflict    = zm_file_row_str(body, "conflict");
    if (conflict.empty())
        conflict = zm_file_conflict::kAsk;
    ZMJSON out =
        co_await m_node->Move(ids, targetSpace, targetDir, conflict, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleCopy(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    int64_t     targetSpace = zm_file_row_int(body, "target_space", 0);
    int64_t     targetDir   = zm_file_row_int(body, "target_dir_id", 0);
    std::string conflict    = zm_file_row_str(body, "conflict");
    if (conflict.empty())
        conflict = zm_file_conflict::kAsk;
    ZmOpCtx ctx = OpOf(gate.ctx, req);

    // 规模阈值:超 500 条或 1GB 转异步任务
    ZMJSON  est   = co_await m_node->EstimateCopy(ids);
    int64_t items = zm_file_row_int(est, "items", 0);
    int64_t bytes = zm_file_row_int(est, "bytes", 0);
    if (items > zm_file::kCopyAsyncItems || bytes > zm_file::kCopyAsyncBytes)
    {
        ZMJSON payload        = ZMJSON::object();
        payload["ids"]        = ids;
        payload["space"]      = targetSpace;
        payload["target_dir"] = targetDir;
        payload["conflict"]   = conflict;
        payload["uid"]        = ctx.uid;
        payload["account"]    = ctx.account;
        payload["ip"]         = ctx.ip;
        ZMJSON created        = co_await m_task->Start(
            zm_file::kTaskCopy, ctx.uid, targetSpace,
            "复制 " + std::to_string(ids.size()) + " 项", "", bytes, items, "",
            [this, ids, targetSpace, targetDir, conflict, ctx](ZmTaskHandle& h)
            {
                ZMJSON r = m_node->CopyExec(ids, targetSpace, targetDir, conflict, ctx, &h);
                if (ZmFileHasError(r))
                    h.Finish(zm_file::kTaskFailed, ZmFileErrorMessage(r));
                else
                    h.Finish(zm_file::kTaskDone, "", r.dump());
            });
        std::string taskNo = zm_file_row_str(created, "task_no");
        if (!taskNo.empty())
            m_task->SetRetryPayload(taskNo, payload);
        ZMJSON out     = ZMJSON::object();
        out["copied"]  = ZMJSON::array();
        out["skipped"] = ZMJSON::array();
        out["failed"]  = ZMJSON::array();
        out["task_no"] = taskNo;
        co_return Respond(out);
    }
    ZMJSON out = co_await m_node->Copy(ids, targetSpace, targetDir, conflict, ctx);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleDelete(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZMJSON out = co_await m_node->SoftDelete(ids, OpOf(gate.ctx, req));
    co_return Respond(out);
}

// ============================================================================
// 回收站
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTrash(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t space = QueryI64(req, "space", 0);
    if (!ZmFileNodeModule::SpaceWritable(space, gate.ctx.uid))
        co_return ZmAuthGateModule::ApiError(403, zm_file_err::kPermDenied, "无权访问该空间");
    ZmListQuery q = ListQueryOf(req, 200);
    q.sort        = QueryStr(req, "sort");
    if (q.sort.empty())
        q.sort = "mtime";
    q.order = QueryStr(req, "order");
    if (q.order.empty())
        q.order = "desc";
    ZMJSON out = co_await m_node->TrashList(space, q, gate.ctx.uid, false);
    if (!ZmFileHasError(out))
        co_await FillTrashNames(out["list"]);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTrashRestore(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZMJSON out = co_await m_node->Restore(ids, OpOf(gate.ctx, req), false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTrashPurge(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZMJSON out = co_await m_node->Purge(ids, OpOf(gate.ctx, req), false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTrashClear(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (auto deny = co_await DenyForeignSpace(space, OpOf(gate.ctx, req), zm_file::kActTrashClear))
        co_return deny;
    ZmOpCtx ctx     = OpOf(gate.ctx, req);
    ZMJSON  created = co_await m_task->Start(
        zm_file::kTaskTrashClear, ctx.uid, space, "清空回收站", "", 0, 0, "",
        [this, space, ctx](ZmTaskHandle& h)
        {
            ZMJSON r = m_node->ClearTrashExec(space, false, ctx, &h);
            h.Finish(zm_file::kTaskDone, "", r.dump());
        });
    std::string taskNo = zm_file_row_str(created, "task_no");
    if (!taskNo.empty())
    {
        ZMJSON payload     = ZMJSON::object();
        payload["space"]   = space;
        payload["uid"]     = ctx.uid;
        payload["account"] = ctx.account;
        payload["ip"]      = ctx.ip;
        m_task->SetRetryPayload(taskNo, payload);
    }
    co_return Respond(created);
}

// ============================================================================
// 上传(元数据走请求头、体为原始字节)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadSimple(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t space = 0;
    int64_t dirId = 0;
    try
    {
        std::string sp = req->getHeader("X-Space");
        space          = sp.empty() ? 0 : std::stoll(sp);
        std::string dp = req->getHeader("X-Dir-Id");
        dirId          = dp.empty() ? 0 : std::stoll(dp);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "上传参数非法");
    }
    std::string name = ZmFileTokenModule::UrlDecode(req->getHeader("X-File-Name"));
    if (name.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少文件名");
    std::string conflict = req->getHeader("X-Conflict");
    if (conflict.empty())
        conflict = zm_file_conflict::kAsk;
    std::string body(req->getBody());
    ZMJSON      out =
        co_await m_upload->Simple(OpOf(gate.ctx, req), space, dirId, name, conflict, body);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadInit(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON      body     = ParseBody(req);
    int64_t     space    = zm_file_row_int(body, "space", 0);
    int64_t     dirId    = zm_file_row_int(body, "dir_id", 0);
    std::string name     = zm_file_row_str(body, "name");
    int64_t     size     = zm_file_row_int(body, "size", 0);
    std::string hash     = zm_file_row_str(body, "hash");
    std::string conflict = zm_file_row_str(body, "conflict");
    if (conflict.empty())
        conflict = zm_file_conflict::kAsk;
    if (name.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少文件名");
    ZMJSON out =
        co_await m_upload->Init(OpOf(gate.ctx, req), space, dirId, name, size, hash, conflict);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadChunk(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    std::string uploadId = req->getHeader("X-Upload-Id");
    int64_t     index    = -1;
    try
    {
        std::string s = req->getHeader("X-Chunk-Index");
        if (!s.empty())
            index = std::stoll(s);
    }
    catch (...)
    {
        index = -1;
    }
    if (uploadId.empty() || index < 0)
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest,
                                             "缺少上传标识或片号");
    std::string body(req->getBody());
    ZMJSON      out = co_await m_upload->PutChunk(OpOf(gate.ctx, req), uploadId, index, body);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadComplete(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON      body     = ParseBody(req);
    std::string uploadId = zm_file_row_str(body, "upload_id");
    if (uploadId.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "缺少上传标识");
    ZMJSON out = co_await m_upload->Complete(OpOf(gate.ctx, req), uploadId);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadStatus(HttpRequestPtr req,
                                                                  std::string    uploadId)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_upload->Status(OpOf(gate.ctx, req), uploadId);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleUploadCancel(HttpRequestPtr req,
                                                                  std::string    uploadId)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_upload->Cancel(OpOf(gate.ctx, req), uploadId);
    co_return Respond(out);
}

// ============================================================================
// 下载与打包
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleDownloadToken(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON      body = ParseBody(req);
    std::string base = SiteBase(req, m_rest);

    // 打包产物:按 task_no 换取直链(压缩包不是 nodes 条目)
    std::string taskNo = zm_file_row_str(body, "task_no");
    if (!taskNo.empty())
    {
        ZMJSON t = co_await m_token->IssuePack(gate.ctx.uid, taskNo);
        if (ZmFileHasError(t))
            co_return Respond(t);
        if (m_pack)
            m_pack->TouchAccess(taskNo);
        ZMJSON out         = ZMJSON::object();
        out["url"]         = ZmFileTokenModule::BuildUrl(base, zm_file_row_str(t, "token"),
                                                         zm_file_row_str(t, "name"));
        out["expire_time"] = zm_file_row_int(t, "expire_time", 0);
        co_return Respond(out);
    }

    std::vector<int64_t> ids = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    if (ids.size() == 1)
    {
        ZMJSON detail = co_await m_node->Detail(ids[0]);
        if (ZmFileHasError(detail))
            co_return Respond(detail);
        if (zm_file_row_int(detail, "type", 0) == zm_file::kTypeFile)
        {
            ZMJSON t = co_await m_token->IssueNode(gate.ctx.uid, ids[0]);
            if (ZmFileHasError(t))
                co_return Respond(t);
            ZMJSON out         = ZMJSON::object();
            out["url"]         = ZmFileTokenModule::BuildUrl(base, zm_file_row_str(t, "token"),
                                                             zm_file_row_str(t, "name"));
            out["expire_time"] = zm_file_row_int(t, "expire_time", 0);
            co_return Respond(out);
        }
    }
    // 多文件或目录:转打包任务
    int64_t space   = zm_file_row_int(co_await m_node->Detail(ids[0]), "space", 0);
    ZMJSON  created = co_await m_pack->Create(space, ids, OpOf(gate.ctx, req));
    co_return Respond(created);
}

drogon::Task<HttpResponsePtr>
ZmFileHubModule::HandleDownload(HttpRequestPtr req, std::string token, std::string filename)
{
    (void)filename;
    ZmTokenTarget target = co_await m_token->Resolve(token);
    if (!target.ok)
        co_return ZmAuthGateModule::ApiError(target.status, target.code, target.message);
    // 发送走"读盘在专用 I/O 池"的流式路径;raw 模式保留 Content-Length(便于显示大小/续传),
    // 块间不设等待(自适应:发完即调度下一块)
    ZmHttpSendFileOptions opts;
    opts.raw          = true;
    opts.chunkSize    = 4 * 1024 * 1024;
    opts.interBlockMs = 0;
    // ETag 带上条目 id:同名同大小同时间的两个条目不应共用缓存标识
    if (target.nodeId > 0)
        opts.etagKey = std::to_string(target.nodeId);
    opts.onFinish     = [this, token]() {
        // 传完 / 客户端断开 / 读失败 / 对端停滞:四条路径都汇到这里,名额在此归还
        m_token->Release(token);
    };
    m_token->TrackTransfer(token, req->getConnectionPtr());
    auto resp = co_await m_rest->SendFileStreamCoro(req, target.path, target.name, opts);
    // 只有"会走流式发送"的响应才由结束回调归还名额:框架在发送阶段才创建流,
    // 而 304 / 416 / 文件缺失等早退路径根本不建流,结束回调不会触发 —— 这些路径立即归还
    if (!resp || !resp->asyncStreamCallback())
        m_token->Release(token);
    if (target.kind == ZmTokenKind::Node || target.kind == ZmTokenKind::Share)
    {
        // 下载行为记审计(只读行为,独立异步写,失败只记警告)
        ZMJSON  row   = co_await m_db->QueryRow("SELECT * FROM nodes WHERE id = ?1",
                                                {std::to_string(target.nodeId)});
        int64_t space = target.kind == ZmTokenKind::Share ? zm_file_row_int(row, "space", 0)
                                                          : zm_file_row_int(row, "space", 0);
        std::string account;
        bool        ranged = !req->getHeader("Range").empty();
        if (target.kind == ZmTokenKind::Node)
            co_await m_audit->RecordDownload(target.uid, account, space, target.nodeId,
                                             target.name, target.fileSize, ranged,
                                             ZmAuthGateModule::ClientIp(req), 1);
    }
    co_return resp;
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandlePack(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    if (auto deny = co_await DenyForeignSpace(space, OpOf(gate.ctx, req), zm_file::kActPack))
        co_return deny;
    std::vector<int64_t> ids = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZMJSON out = co_await m_pack->Create(space, ids, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandlePackClean(HttpRequestPtr req,
                                                               std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_pack->CleanTask(taskNo, gate.ctx.uid, false);
    co_return Respond(out);
}

// ============================================================================
// 传输任务
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTasks(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int    type   = QueryInt(req, "type", 0, 0, 99);
    int    status = QueryInt(req, "status", 0, 0, 99);
    int    page   = QueryInt(req, "page", 1, 1, 1000000);
    int    size   = QueryInt(req, "size", 20, 1, 200);
    ZMJSON out    = co_await m_task->List(gate.ctx.uid, type, status, page, size);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTasksActive(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_task->Active(gate.ctx.uid, false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTaskDetail(HttpRequestPtr req,
                                                                std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_task->Detail(taskNo, gate.ctx.uid, false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTaskCancel(HttpRequestPtr req,
                                                                std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_task->Cancel(taskNo, gate.ctx.uid, false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTaskRetry(HttpRequestPtr req,
                                                               std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_task->Retry(taskNo, gate.ctx.uid);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTaskDelete(HttpRequestPtr req,
                                                                std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    // 只删记录,不动打包产物:压缩包是缓存文件,由缓存回收统一处理(空闲 30 分钟 /
    // 每日兜底 / 容量阈值),与"清除历史"同一口径 —— 记录与产物各有各的生命周期
    ZMJSON out = co_await m_task->Delete(taskNo, gate.ctx.uid, false);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleTasksClear(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON body   = ParseBody(req);
    int    status = static_cast<int>(zm_file_row_int(body, "status", 0));
    ZMJSON out    = co_await m_task->Clear(gate.ctx.uid, status, false);
    co_return Respond(out);
}

// ============================================================================
// 分享(登录侧)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareCreate(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = zm_file_row_int(body, "space", 0);
    // 多选分享传 node_ids 数组;兼容旧形态的单 node_id
    std::vector<int64_t> nodeIds = BodyIds(body, "node_ids");
    if (nodeIds.empty())
    {
        int64_t nodeId = zm_file_row_int(body, "node_id", 0);
        if (nodeId > 0)
            nodeIds.push_back(nodeId);
    }
    if (nodeIds.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定分享目标");
    bool    pwdEnabled = zm_json_get_bool(body, "pwd_enabled", false);
    int64_t expireDays = zm_file_row_int(body, "expire_days", 0);
    int64_t expireTime = zm_file_row_int(body, "expire_time", 0);
    int64_t maxDl      = zm_file_row_int(body, "max_downloads", 0);
    bool    loginOnly  = zm_json_get_bool(body, "login_only", false);
    ZMJSON  out = co_await m_share->Create(OpOf(gate.ctx, req), space, nodeIds, pwdEnabled,
                                           expireDays, expireTime, maxDl, loginOnly);
    if (!ZmFileHasError(out))
        out["url"] = ZmFileShareModule::BuildShareUrl(SitePageBase(req, m_rest),
                                                      zm_file_row_str(out, "token"));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareList(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int    status = QueryInt(req, "status", 0, 0, 99);
    int    page   = QueryInt(req, "page", 1, 1, 1000000);
    int    size   = QueryInt(req, "size", 50, 1, 200);
    ZMJSON out    = co_await m_share->List(gate.ctx.uid, status, page, size);
    if (!ZmFileHasError(out))
    {
        // 分享链接用页面基址(80/443),不能用 REST 端口;详见 SitePageBase 注释
        std::string base = SitePageBase(req, m_rest);
        for (auto& s : out["list"])
            s["url"] = ZmFileShareModule::BuildShareUrl(base, zm_file_row_str(s, "token"));
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleSharePatch(HttpRequestPtr req,
                                                                std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "分享 id 非法");
    }
    ZMJSON out = co_await m_share->Patch(gate.ctx.uid, id, ParseBody(req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareCancel(HttpRequestPtr req,
                                                                 std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "分享 id 非法");
    }
    ZMJSON out = co_await m_share->Cancel(gate.ctx.uid, id, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareResume(HttpRequestPtr req,
                                                                std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "分享 id 非法");
    }
    ZMJSON out = co_await m_share->Resume(gate.ctx.uid, id, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleSharePurge(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON body = ParseBody(req);
    // 两种用法:{ids:[...]} 删指定记录;{inactive:true} 清空全部非有效记录
    bool                 inactiveOnly = zm_json_get_bool(body, "inactive", false);
    std::vector<int64_t> ids          = BodyIds(body);
    ZMJSON out = co_await m_share->Purge(gate.ctx.uid, ids, inactiveOnly, OpOf(gate.ctx, req));
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareLogs(HttpRequestPtr req,
                                                               std::string    idStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t id = 0;
    try
    {
        id = std::stoll(idStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "分享 id 非法");
    }
    int    page = QueryInt(req, "page", 1, 1, 1000000);
    int    size = QueryInt(req, "size", 20, 1, 200);
    ZMJSON out  = co_await m_share->Logs(gate.ctx.uid, id, page, size);
    co_return Respond(out);
}

// ============================================================================
// 分享(公开面,免会话)
// ============================================================================
void ZmFileHubModule::SetShareCookie(const HttpResponsePtr& resp, const std::string& token,
                                     const std::string& cred)
{
    (void)token;
    drogon::Cookie c("zm_share", cred);
    c.setPath("/");
    c.setHttpOnly(true);
    c.setSameSite(drogon::Cookie::SameSite::kLax);
    c.setMaxAge(static_cast<int>(zm_file::kShareCredTtlSec));
    resp->addCookie(std::move(c));
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareInfo(HttpRequestPtr req,
                                                               std::string    token)
{
    // 免会话:登录态可有可无(login_only 的分享需要它)
    int64_t viewerUid = 0;
    {
        const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
        if (!cookie.empty())
        {
            auto ctx =
                co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
            if (ctx.valid)
                viewerUid = ctx.uid;
        }
    }
    ZMJSON out = co_await m_share->Info(token, viewerUid, ZmAuthGateModule::ClientIp(req),
                                        req->getHeader("User-Agent"));
    if (!ZmFileHasError(out))
    {
        ZMJSON list = ZMJSON::array();
        list.push_back(out);
        co_await FillOwnerNames(list);
        out = list[0];
    }
    auto resp = Respond(out);
    // 需要提取码时下发访问凭证 Cookie(前端零处理)
    if (!ZmFileHasError(out) && zm_file_row_str(out, "need_pwd") == "false")
    {
    }
    co_return resp;
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareVerify(HttpRequestPtr req,
                                                                 std::string    token)
{
    ZMJSON      body = ParseBody(req);
    std::string pwd  = zm_file_row_str(body, "pwd");
    ZMJSON      out  = co_await m_share->Verify(token, pwd, ZmAuthGateModule::ClientIp(req),
                                                req->getHeader("User-Agent"));
    auto        resp = Respond(out);
    if (!ZmFileHasError(out) && zm_json_get_bool(out, "pass", false))
        SetShareCookie(resp, token, zm_file_row_str(out, "cred"));
    co_return resp;
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareListDir(HttpRequestPtr req,
                                                                  std::string    token)
{
    std::string cred      = req->getCookie("zm_share");
    bool        credOk    = m_share->CreditValid(token, cred);
    int64_t     dirId     = QueryI64(req, "dir_id", 0);
    ZmListQuery q         = ListQueryOf(req, 200);
    int64_t     viewerUid = 0;
    {
        const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
        if (!cookie.empty())
        {
            auto ctx =
                co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
            if (ctx.valid)
                viewerUid = ctx.uid;
        }
    }
    ZMJSON out = co_await m_share->ListDir(token, dirId, q, credOk, viewerUid,
                                           ZmAuthGateModule::ClientIp(req),
                                           req->getHeader("User-Agent"));
    if (!ZmFileHasError(out))
        co_await FillOwnerNames(out["list"]);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileHubModule::HandleShareDownload(HttpRequestPtr req,
                                                                   std::string    token)
{
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    std::string cred      = req->getCookie("zm_share");
    bool        credOk    = m_share->CreditValid(token, cred);
    int64_t     viewerUid = 0;
    {
        const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
        if (!cookie.empty())
        {
            auto ctx =
                co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
            if (ctx.valid)
                viewerUid = ctx.uid;
        }
    }
    ZMJSON out = co_await m_share->Download(token, ids, credOk, viewerUid,
                                            ZmAuthGateModule::ClientIp(req),
                                            req->getHeader("User-Agent"));
    // 单文件:换取的是令牌,拼直链返回(与登录侧同构)
    if (!ZmFileHasError(out) && out.contains("token"))
    {
        std::string base = SiteBase(req, m_rest);
        ZMJSON      r    = ZMJSON::object();
        r["url"]         = ZmFileTokenModule::BuildUrl(base, zm_file_row_str(out, "token"),
                                                       zm_file_row_str(out, "name"));
        r["expire_time"] = zm_file_row_int(out, "expire_time", 0);
        co_return Respond(r);
    }
    co_return Respond(out);
}
