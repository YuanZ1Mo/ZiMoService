#include "modules/module_file_share.h"

#include <openssl/hmac.h>

#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"
#include "modules/module_file_token.h"
#include "modules/module_file_pack.h"

#include "service_define.h" // ZM_FILE_HUB_HMAC_KEY

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <random>
#include <set>

using namespace drogon;

namespace
{
/// 提取码失败计数的作用域(通用计数表 write_limits)
constexpr const char* kScopePwdFail = "share_pwd_fail";

/// 字节 → 十六进制小写
std::string ToHex(const unsigned char* data, unsigned int len)
{
    static const char* hex = "0123456789abcdef";
    std::string        s;
    s.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i)
    {
        s += hex[data[i] >> 4];
        s += hex[data[i] & 0x0F];
    }
    return s;
}

/// 随机十六进制串
std::string RandomHex(int bytes)
{
    static const char*                 hex = "0123456789abcdef";
    std::random_device                 rd;
    std::mt19937_64                    gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string                        s;
    s.reserve(static_cast<size_t>(bytes) * 2);
    for (int i = 0; i < bytes * 2; ++i)
        s += hex[dist(gen)];
    return s;
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileShareModule::ZmFileShareModule(ZmFileDbModule* db, ZmFileNodeModule* node,
                                     ZmFileStoreModule* store, ZmFileAuditModule* audit,
                                     ZmFileTokenModule* token, ZmFilePackModule* pack)
    : m_db(db), m_node(node), m_store(store), m_audit(audit), m_token(token), m_pack(pack)
{
}

ZmFileShareModule::~ZmFileShareModule() = default;

// ============================================================================
// 提取码与凭证
// ============================================================================
std::string ZmFileShareModule::GeneratePwd()
{
    // 去掉易混淆的 0 O 1 l I
    static const char* cs = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::mt19937       gen(rd());
    std::uniform_int_distribution<int> dist(0, 56);
    std::string                        s;
    for (int i = 0; i < 4; ++i)
        s += cs[dist(gen)];
    return s;
}

std::string ZmFileShareModule::HashPwd(const std::string& token, const std::string& pwd)
{
    // 只存哈希;盐用分享 token(同一提取码在不同分享下哈希不同)
    std::string   data = token + "|" + pwd;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLen = 0;
    HMAC(EVP_sha256(), ZM_FILE_HUB_HMAC_KEY,
         static_cast<int>(std::strlen(ZM_FILE_HUB_HMAC_KEY)),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &mdLen);
    return ToHex(md, mdLen);
}

std::string ZmFileShareModule::BuildShareUrl(const std::string& base, const std::string& token)
{
    return base + "/s/" + token;
}

bool ZmFileShareModule::CreditValid(const std::string& token, const std::string& cred)
{
    if (cred.empty())
        return false;
    std::lock_guard<std::mutex> lk(m_credMtx);
    auto                        it = m_creds.find(token);
    if (it == m_creds.end())
        return false;
    if (it->second.expire < ZmSqliteDb::Now())
    {
        m_creds.erase(it);
        return false;
    }
    return it->second.value == cred;
}

// ============================================================================
// 行读取与可用性判定
// ============================================================================
ZMJSON ZmFileShareModule::LoadShareSync(int64_t shareId)
{
    return m_db->QueryRowSync("SELECT * FROM shares WHERE id = ?1", {std::to_string(shareId)});
}

int ZmFileShareModule::CheckUsableSync(const ZMJSON& share, std::string& code,
                                       std::string& message)
{
    if (share.empty())
    {
        code    = zm_file_err::kShareNotFound;
        message = "分享不存在或链接已失效";
        return 404;
    }
    int status = static_cast<int>(zm_file_row_int(share, "status", 0));
    if (status == zm_file::kShareCanceled)
    {
        code    = zm_file_err::kShareExpired;
        message = "分享已被取消";
        return 410;
    }
    if (status == zm_file::kShareInvalid)
    {
        code    = zm_file_err::kShareExpired;
        message = "分享已失效";
        return 410;
    }
    int64_t expire = zm_file_row_int(share, "expire_time", 0);
    if (expire > 0 && expire < ZmSqliteDb::Now())
    {
        code    = zm_file_err::kShareExpired;
        message = "分享已过期";
        return 410;
    }
    int64_t maxDl = zm_file_row_int(share, "max_downloads", 0);
    int64_t done  = zm_file_row_int(share, "download_count", 0);
    if (maxDl > 0 && done >= maxDl)
    {
        code    = zm_file_err::kShareExpired;
        message = "分享下载次数已达上限";
        return 410;
    }
    // 目标在回收站/不存在:分享行状态不变,恢复后自动可用
    int64_t nodeId = zm_file_row_int(share, "node_id", 0);
    ZMJSON  row    = m_db->NodeRowSync(nodeId);
    bool    visible =
        !row.empty() && zm_file_row_int(row, "deleted", 0) == 0 && m_node->VisibleSync(nodeId);
    if (!visible)
    {
        code    = zm_file_err::kShareUnavailable;
        message = "分享内容暂不可用(可能已被删除)";
        return 404;
    }
    return 0;
}

bool ZmFileShareModule::InShareTreeSync(int64_t dirId, int64_t shareRootId)
{
    if (dirId == 0 || dirId == shareRootId)
        return true;
    ZMJSON row = m_db->QueryRowSync(
        "WITH RECURSIVE up(id, parent_id, depth) AS ("
        " SELECT id, parent_id, 0 FROM nodes WHERE id = ?1"
        " UNION ALL"
        " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n JOIN up ON n.id = up.parent_id"
        " WHERE up.depth < 64)"
        " SELECT COUNT(*) AS n FROM up WHERE id = ?2;",
        {std::to_string(dirId), std::to_string(shareRootId)});
    return zm_file_row_int(row, "n", 0) > 0;
}

// ============================================================================
// 创建 / 列表 / 修改 / 取消 / 日志
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Create(const ZmOpCtx& ctx, int64_t space,
                                               int64_t nodeId, bool pwdEnabled,
                                               int64_t expireDays, int64_t expireTime,
                                               int64_t maxDownloads, bool loginOnly)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, space, nodeId, pwdEnabled, expireDays, expireTime, maxDownloads,
         loginOnly]() -> ZMJSON
        {
            (void)space;
            ZMJSON row;
            if (!m_node->VisibleSync(nodeId, row))
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
            int64_t realSpace = zm_file_row_int(row, "space", 0);
            if (!ZmFileNodeModule::SpaceWritable(realSpace, ctx.uid))
                return ZmFileError(zm_file_err::kPermDenied, 403, "无权分享该空间的条目");
            ZMJSON cnt = m_db->QueryRowSync(
                "SELECT COUNT(*) AS n FROM shares WHERE uid = ?1 AND status = 1",
                {std::to_string(ctx.uid)});
            if (zm_file_row_int(cnt, "n", 0) >= zm_file::kShareMaxPerUser)
                return ZmFileError(zm_file_err::kTooManyShares, 429,
                                   "有效分享数已达上限(200),请先取消部分分享");

            int64_t now    = ZmSqliteDb::Now();
            int64_t expire = 0;
            if (expireTime > 0)
                expire = expireTime;
            else if (expireDays > 0)
                expire = now + expireDays * 86400;
            std::string token = RandomHex(16);
            std::string pwdPlain;
            std::string pwdHash;
            if (pwdEnabled)
            {
                pwdPlain = GeneratePwd();
                pwdHash  = HashPwd(token, pwdPlain);
            }
            int64_t     nodeType = zm_file_row_int(row, "type", 0);
            std::string name     = zm_file_row_str(row, "name");
            int64_t     shareId  = 0;
            bool        ok       = m_db->WithTxSync(
                [&](ZmSqliteDb& db) -> bool
                {
                    if (!db.ExecSync(
                            "INSERT INTO "
                                         "shares(token,uid,space,node_id,node_type,name,pwd_hash,"
                                         "login_only,expire_time,max_downloads,download_count,view_count,"
                                         "status,"
                                         "create_time,update_time) "
                                         "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,0,1,?11,?11)",
                            {token, std::to_string(ctx.uid), std::to_string(realSpace),
                             std::to_string(nodeId), std::to_string(nodeType), name, pwdHash,
                             std::to_string(loginOnly ? 1 : 0), std::to_string(expire),
                             std::to_string(maxDownloads), std::to_string(now)}))
                        return false;
                    shareId = zm_file_row_int(
                        db.QueryRowTxSync("SELECT last_insert_rowid() AS id", {}), "id", 0);
                    ZMJSON detail         = ZMJSON::object();
                    detail["token"]       = token;
                    detail["path"]        = m_node->RelPathSync(nodeId);
                    detail["expire_time"] = expire;
                    detail["pwd"]         = pwdEnabled;
                    return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                                  zm_file::kActShareCreate, realSpace,
                                                                  nodeId, name, detail.dump(), ctx.ip, 1);
                });
            if (!ok)
                return ZmFileError(zm_file_err::kInternal, 500, "创建分享失败");
            ZMJSON out         = ZMJSON::object();
            out["share_id"]    = shareId;
            out["token"]       = token;
            out["pwd"]         = pwdPlain;
            out["expire_time"] = expire;
            return out;
        });
}

ZMJSON ShareViewOf(const ZMJSON& row)
{
    ZMJSON j            = ZMJSON::object();
    j["id"]             = zm_file_row_int(row, "id", 0);
    j["token"]          = zm_file_row_str(row, "token");
    j["name"]           = zm_file_row_str(row, "name");
    j["node_type"]      = zm_file_row_int(row, "node_type", 0);
    j["node_id"]        = zm_file_row_int(row, "node_id", 0);
    j["space"]          = zm_file_row_int(row, "space", 0);
    j["has_pwd"]        = !zm_file_row_str(row, "pwd_hash").empty();
    j["login_only"]     = zm_file_row_int(row, "login_only", 0);
    j["expire_time"]    = zm_file_row_int(row, "expire_time", 0);
    j["max_downloads"]  = zm_file_row_int(row, "max_downloads", 0);
    j["download_count"] = zm_file_row_int(row, "download_count", 0);
    j["view_count"]     = zm_file_row_int(row, "view_count", 0);
    j["status"]         = zm_file_row_int(row, "status", 0);
    // 状态文案由服务端下发(界面直接渲染,避免前后端各写一份映射)
    {
        int st = static_cast<int>(zm_file_row_int(row, "status", 0));
        j["status_name"] = st == zm_file::kShareActive ? "有效"
                           : st == zm_file::kShareCanceled ? "已取消"
                                                           : "已失效";
    }
    j["create_time"]    = zm_file_row_int(row, "create_time", 0);
    j["update_time"]    = zm_file_row_int(row, "update_time", 0);
    return j;
}

drogon::Task<ZMJSON> ZmFileShareModule::List(int64_t uid, int status, int page, int size)
{
    std::string              where = " WHERE uid = ?1";
    std::vector<std::string> p     = {std::to_string(uid)};
    if (status > 0)
    {
        where += " AND status = ?2";
        p.push_back(std::to_string(status));
    }
    ZMJSON  totalRow = co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM shares" + where, p);
    int64_t total    = zm_file_row_int(totalRow, "n", 0);
    std::vector<std::string> lp = p;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string((page - 1) * size));
    ZMJSON rows = co_await m_db->QueryRows(
        "SELECT * FROM shares" + where + " ORDER BY id DESC LIMIT ?" +
            std::to_string(lp.size() - 1) + " OFFSET ?" + std::to_string(lp.size()),
        lp);
    // 过期未置位的分享在列表里按"已失效"展示(周期清理会落库)
    int64_t now  = ZmSqliteDb::Now();
    ZMJSON  list = ZMJSON::array();
    for (const auto& r : rows)
    {
        ZMJSON  v      = ShareViewOf(r);
        int64_t expire = zm_file_row_int(v, "expire_time", 0);
        int64_t maxDl  = zm_file_row_int(v, "max_downloads", 0);
        int64_t done   = zm_file_row_int(v, "download_count", 0);
        if (zm_file_row_int(v, "status", 0) == 1 &&
            ((expire > 0 && expire < now) || (maxDl > 0 && done >= maxDl)))
            v["status"] = 3;
        list.push_back(std::move(v));
    }
    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return out;
}

drogon::Task<ZMJSON> ZmFileShareModule::Patch(int64_t uid, int64_t shareId, const ZMJSON& body)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE id = ?1 AND uid = ?2",
                                         {std::to_string(shareId), std::to_string(uid)});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kShareNotFound, 404, "分享不存在");
    int64_t                  now       = ZmSqliteDb::Now();
    std::string              token     = zm_file_row_str(row, "token");
    std::string              setClause = "update_time = ?1";
    std::vector<std::string> params    = {std::to_string(now)};
    std::string              newPwd;
    auto                     addSet = [&](const std::string& col, const std::string& val)
    {
        setClause += ", " + col + " = ?" + std::to_string(params.size() + 1);
        params.push_back(val);
    };
    if (body.contains("expire_days"))
    {
        int64_t days = zm_file_row_int(body, "expire_days", 0);
        addSet("expire_time", std::to_string(days > 0 ? now + days * 86400 : 0));
    }
    if (body.contains("max_downloads"))
        addSet("max_downloads", std::to_string(zm_file_row_int(body, "max_downloads", 0)));
    if (body.contains("login_only"))
        addSet("login_only", std::to_string(zm_file_row_int(body, "login_only", 0) ? 1 : 0));
    if (zm_json_get_bool(body, "reset_pwd", false))
    {
        newPwd = GeneratePwd();
        addSet("pwd_hash", HashPwd(token, newPwd));
    }
    params.push_back(std::to_string(shareId));
    bool ok = co_await m_db->Exec("UPDATE shares SET " + setClause + " WHERE id = ?" +
                                      std::to_string(params.size()),
                                  params);
    if (!ok)
        co_return ZmFileError(zm_file_err::kInternal, 500, "修改分享失败");
    ZMJSON out = ZMJSON::object();
    if (!newPwd.empty())
        out["pwd"] = newPwd;
    co_return out;
}

drogon::Task<ZMJSON> ZmFileShareModule::Cancel(int64_t uid, int64_t shareId,
                                               const ZmOpCtx& ctx)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE id = ?1 AND uid = ?2",
                                         {std::to_string(shareId), std::to_string(uid)});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kShareNotFound, 404, "分享不存在");
    int64_t     now    = ZmSqliteDb::Now();
    int64_t     space  = zm_file_row_int(row, "space", 0);
    int64_t     nodeId = zm_file_row_int(row, "node_id", 0);
    std::string name   = zm_file_row_str(row, "name");
    bool        ok     = co_await m_db->WithTx(
        [&](ZmSqliteDb& db) -> bool
        {
            if (!db.ExecSync("UPDATE shares SET status = ?1, update_time = ?2 WHERE id = ?3",
                                        {std::to_string(zm_file::kShareCanceled), std::to_string(now),
                              std::to_string(shareId)}))
                return false;
            ZMJSON detail   = ZMJSON::object();
            detail["token"] = zm_file_row_str(row, "token");
            return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                        zm_file::kActShareCancel, space, nodeId, name,
                                                        detail.dump(), ctx.ip, 1);
        });
    if (!ok)
        co_return ZmFileError(zm_file_err::kInternal, 500, "取消分享失败");
    co_return ZMJSON::object();
}

drogon::Task<ZMJSON> ZmFileShareModule::Logs(int64_t uid, int64_t shareId, int page, int size)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT id FROM shares WHERE id = ?1 AND uid = ?2",
                                         {std::to_string(shareId), std::to_string(uid)});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kShareNotFound, 404, "分享不存在");
    co_return co_await m_audit->QueryShareLogsByShare(shareId, page, size);
}

// ============================================================================
// 公开面:分享信息
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Info(const std::string& token, int64_t viewerUid,
                                             const std::string& ip, const std::string& ua)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE token = ?1", {token});
    std::string code;
    std::string message;
    int         status    = 0;
    int64_t     shareId   = 0;
    bool        needLogin = false;
    ZMJSON      out       = ZMJSON::object();
    int64_t     ownerUid  = 0;
    {
        // 可用性判定与计数在同一段同步逻辑里做,避免中间被改
        ZMJSON result = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, viewerUid, token, &code, &message, &status, &ownerUid]() -> ZMJSON
            {
                ZMJSON o = ZMJSON::object();
                status   = CheckUsableSync(row, code, message);
                if (status != 0)
                    return o;
                ownerUid      = zm_file_row_int(row, "uid", 0);
                int loginOnly = static_cast<int>(zm_file_row_int(row, "login_only", 0));
                if (loginOnly == 1 && viewerUid == 0)
                {
                    status          = 401;
                    code            = "NEED_LOGIN";
                    message         = "该分享需要登录后访问";
                    o["need_login"] = true;
                    return o;
                }
                // 浏览计数(view_count 单条原子自增)
                m_db->ExecSync("UPDATE shares SET view_count = view_count + 1 WHERE id = ?1",
                               {std::to_string(zm_file_row_int(row, "id", 0))});
                o["ok"] = true;
                return o;
            });
        if (!zm_json_get_bool(result, "ok", false))
        {
            if (status == 401)
            {
                ZMJSON err                 = ZmFileError("NEED_LOGIN", 401, message);
                err["error"]["need_login"] = true;
                co_return err;
            }
            co_return ZmFileError(code.empty() ? zm_file_err::kShareNotFound : code.c_str(),
                                  status == 0 ? 404 : status, message);
        }
    }
    shareId = zm_file_row_int(row, "id", 0);
    if (m_audit)
        co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogView, 1,
                                            viewerUid, ip, ua, "");

    out["name"]           = zm_file_row_str(row, "name");
    out["node_type"]      = zm_file_row_int(row, "node_type", 0);
    out["need_pwd"]       = !zm_file_row_str(row, "pwd_hash").empty();
    out["expired"]        = false;
    out["expire_time"]    = zm_file_row_int(row, "expire_time", 0);
    out["owner_uid"]      = ownerUid;
    out["login_only"]     = zm_file_row_int(row, "login_only", 0);
    out["need_login"]     = needLogin;
    out["max_downloads"]  = zm_file_row_int(row, "max_downloads", 0);
    out["download_count"] = zm_file_row_int(row, "download_count", 0);
    out["node_id"]        = zm_file_row_int(row, "node_id", 0);
    co_return out;
}

// ============================================================================
// 公开面:提取码校验
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Verify(const std::string& token,
                                               const std::string& pwd, const std::string& ip,
                                               const std::string& ua)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, token, pwd, ip, ua]() -> ZMJSON
        {
            ZMJSON row = m_db->QueryRowSync("SELECT * FROM shares WHERE token = ?1", {token});
            if (row.empty())
                return ZmFileError(zm_file_err::kShareNotFound, 404, "分享不存在或链接已失效");
            int64_t     shareId = zm_file_row_int(row, "id", 0);
            std::string code;
            std::string message;
            int         status = CheckUsableSync(row, code, message);
            if (status != 0)
                return ZmFileError(code.c_str(), status, message);

            std::string stored = zm_file_row_str(row, "pwd_hash");
            if (stored.empty())
            {
                // 未设提取码:直接签发凭证
                ZMJSON      out  = ZMJSON::object();
                std::string cred = RandomHex(24);
                {
                    std::lock_guard<std::mutex> lk(m_credMtx);
                    m_creds[token] = {cred, ZmSqliteDb::Now() + zm_file::kShareCredTtlSec};
                }
                out["pass"] = true;
                out["cred"] = cred;
                return out;
            }

            // 冷却检查:同 (IP, token) 连续错 5 次 → 冷却 10 分钟
            std::string dimKey = ip + "|" + token;
            int64_t     now    = ZmSqliteDb::Now();
            int64_t     count  = ZmFileDbModule::LimitCountSync(*m_db, kScopePwdFail, dimKey);
            int64_t     windowStart =
                ZmFileDbModule::LimitWindowStartSync(*m_db, kScopePwdFail, dimKey);
            if (count >= zm_file::kSharePwdFailMax &&
                (now - windowStart) < zm_file::kSharePwdCoolSec)
            {
                int64_t remain = zm_file::kSharePwdCoolSec - (now - windowStart);
                return ZmFileError(zm_file_err::kShareLocked, 429,
                                   "提取码错误次数过多,请 " +
                                       std::to_string((remain + 59) / 60) + " 分钟后再试");
            }

            bool pass = (HashPwd(token, pwd) == stored);
            if (!pass)
            {
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    {
                        return ZmFileDbModule::BumpLimitSync(db, kScopePwdFail, dimKey,
                                                             zm_file::kSharePwdCoolSec, now);
                    });
            }
            else
            {
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    { return ZmFileDbModule::ClearLimitSync(db, kScopePwdFail, dimKey); });
            }
            ZMJSON out  = ZMJSON::object();
            out["pass"] = pass;
            if (pass)
            {
                std::string cred = RandomHex(24);
                {
                    std::lock_guard<std::mutex> lk(m_credMtx);
                    m_creds[token] = {cred, now + zm_file::kShareCredTtlSec};
                }
                out["cred"] = cred;
            }
            // 日志(成功/失败都记)
            (void)shareId;
            return out;
        });
}

// ============================================================================
// 公开面:列目录
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::ListDir(const std::string& token, int64_t dirId,
                                                const ZmListQuery& q, bool credOk,
                                                int64_t viewerUid, const std::string& ip,
                                                const std::string& ua)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE token = ?1", {token});
    std::string code;
    std::string message;
    int64_t     shareId = 0;
    int64_t     rootId  = 0;
    int64_t     space   = 0;
    {
        ZMJSON gate = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, dirId, credOk, &code, &message]() -> ZMJSON
            {
                ZMJSON o      = ZMJSON::object();
                int    status = CheckUsableSync(row, code, message);
                if (status != 0)
                {
                    o["status"] = status;
                    return o;
                }
                if (!zm_file_row_str(row, "pwd_hash").empty() && !credOk)
                {
                    code        = "NEED_PWD";
                    message     = "请先输入提取码";
                    o["status"] = 403;
                    return o;
                }
                int64_t root = zm_file_row_int(row, "node_id", 0);
                if (!InShareTreeSync(dirId, root))
                {
                    code        = zm_file_err::kNodeNotFound;
                    message     = "目录不在分享范围内";
                    o["status"] = 404;
                    return o;
                }
                o["ok"]   = true;
                o["root"] = root;
                return o;
            });
        if (!zm_json_get_bool(gate, "ok", false))
            co_return ZmFileError(
                code.c_str(), static_cast<int>(zm_file_row_int(gate, "status", 404)), message);
        shareId = zm_file_row_int(row, "id", 0);
        rootId  = zm_file_row_int(gate, "root", 0);
        space   = zm_file_row_int(row, "space", 0);
    }
    // 分享根用 0 表示;其余用实际条目 id 列目录
    int64_t baseId = (dirId == 0) ? rootId : dirId;
    ZMJSON  list   = co_await m_node->List(space, baseId, q);
    if (ZmFileHasError(list))
    {
        if (m_audit)
            co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogList, 2,
                                                viewerUid, ip, ua, "列目录失败");
        co_return list;
    }
    if (m_audit)
        co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogList, 1,
                                            viewerUid, ip, ua, "");
    // 面包屑:从分享根到当前目录(前端只认这棵树)
    ZMJSON bc = ZMJSON::array();
    {
        ZMJSON rootRow   = co_await m_db->QueryRow("SELECT id, name FROM nodes WHERE id = ?1",
                                                   {std::to_string(rootId)});
        ZMJSON rootItem  = ZMJSON::object();
        rootItem["id"]   = zm_file_row_int(rootRow, "id", 0);
        rootItem["name"] = zm_file_row_str(rootRow, "name");
        bc.push_back(std::move(rootItem));
        if (baseId != rootId)
        {
            ZMJSON chain = co_await m_db->QueryRows(
                "WITH RECURSIVE up(id, parent_id, name, depth) AS ("
                " SELECT id, parent_id, name, 0 FROM nodes WHERE id = ?1"
                " UNION ALL"
                " SELECT n.id, n.parent_id, n.name, up.depth + 1 FROM nodes n"
                " JOIN up ON n.id = up.parent_id WHERE up.depth < 64)"
                " SELECT id, name FROM up WHERE depth >= 1 ORDER BY depth DESC;",
                {std::to_string(baseId)});
            // 只保留分享根之后的部分
            bool afterRoot = false;
            for (const auto& r : chain)
            {
                if (afterRoot)
                    bc.push_back(r);
                if (zm_file_row_int(r, "id", 0) == rootId)
                    afterRoot = true;
            }
        }
    }
    if (!list.is_object())
        co_return ZmFileError(zm_file_err::kInternal, 500, "列目录失败");
    list["breadcrumb"] = std::move(bc);
    co_return list;
}

// ============================================================================
// 公开面:下载
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Download(const std::string&          token,
                                                 const std::vector<int64_t>& ids, bool credOk,
                                                 int64_t viewerUid, const std::string& ip,
                                                 const std::string& ua)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE token = ?1", {token});
    std::string code;
    std::string message;
    int64_t     shareId  = 0;
    int64_t     rootId   = 0;
    int64_t     space    = 0;
    int64_t     ownerUid = 0;
    {
        ZMJSON gate = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, ids, credOk, &code, &message]() -> ZMJSON
            {
                ZMJSON o      = ZMJSON::object();
                int    status = CheckUsableSync(row, code, message);
                if (status != 0)
                {
                    o["status"] = status;
                    return o;
                }
                if (!zm_file_row_str(row, "pwd_hash").empty() && !credOk)
                {
                    code        = "NEED_PWD";
                    message     = "请先输入提取码";
                    o["status"] = 403;
                    return o;
                }
                int64_t root = zm_file_row_int(row, "node_id", 0);
                for (int64_t id : ids)
                {
                    if (!InShareTreeSync(id, root))
                    {
                        code        = zm_file_err::kNodeNotFound;
                        message     = "条目不在分享范围内";
                        o["status"] = 404;
                        return o;
                    }
                }
                o["ok"]   = true;
                o["root"] = root;
                return o;
            });
        if (!zm_json_get_bool(gate, "ok", false))
            co_return ZmFileError(
                code.c_str(), static_cast<int>(zm_file_row_int(gate, "status", 404)), message);
        shareId  = zm_file_row_int(row, "id", 0);
        rootId   = zm_file_row_int(gate, "root", 0);
        space    = zm_file_row_int(row, "space", 0);
        ownerUid = zm_file_row_int(row, "uid", 0);
    }

    // 单文件:直接换取下载令牌
    if (ids.size() == 1)
    {
        ZMJSON detail = co_await m_node->Detail(ids[0]);
        if (ZmFileHasError(detail))
            co_return detail;
        if (zm_file_row_int(detail, "type", 0) == zm_file::kTypeFile)
        {
            ZMJSON t = co_await m_token->IssueShare(token, ids[0]);
            if (ZmFileHasError(t))
                co_return t;
            // 下载计数用单条原子自增(免登录接口可能高并发命中同一行)
            co_await m_db->Exec(
                "UPDATE shares SET download_count = download_count + 1 WHERE id = ?1",
                {std::to_string(shareId)});
            if (m_audit)
                co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogDownload,
                                                    1, viewerUid, ip, ua,
                                                    zm_file_row_str(detail, "name"));
            ZMJSON out         = ZMJSON::object();
            out["token"]       = zm_file_row_str(t, "token");
            out["expire_time"] = zm_file_row_int(t, "expire_time", 0);
            out["name"]        = zm_file_row_str(t, "name");
            co_return out;
        }
    }

    // 多条目 / 目录:打包(同一请求重复调用幂等)
    std::string key = token + "|";
    {
        std::vector<int64_t> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        for (int64_t id : sorted)
            key += std::to_string(id) + ",";
    }
    std::string existTask;
    {
        std::lock_guard<std::mutex> lk(m_packMtx);
        auto                        it = m_packKeys.find(key);
        if (it != m_packKeys.end())
            existTask = it->second;
    }
    if (!existTask.empty())
    {
        ZMJSON trow = co_await m_db->QueryRow(
            "SELECT * FROM transfer_tasks WHERE task_no = ?1", {existTask});
        if (!trow.empty())
        {
            int st = static_cast<int>(zm_file_row_int(trow, "status", 0));
            if (st == zm_file::kTaskDone)
            {
                std::string zipName = zm_file_row_str(trow, "result");
                if (!zipName.empty())
                {
                    // 压缩包是否还在(可能已被缓存清理)
                    bool exists = co_await ZmHttpServer::RunOnPool<bool>(
                        [this, space, zipName]() -> bool
                        { return m_store->Exists(m_store->ZipDir(space) + "\\" + zipName); });
                    if (exists)
                    {
                        ZMJSON t = co_await m_token->IssuePack(ownerUid, existTask);
                        if (!ZmFileHasError(t))
                        {
                            co_await m_db->Exec(
                                "UPDATE shares SET download_count = download_count + 1 "
                                "WHERE id = ?1",
                                {std::to_string(shareId)});
                            if (m_audit)
                                co_await m_audit->RecordShareAccess(
                                    shareId, token, zm_file::kShareLogPack, 1, viewerUid, ip,
                                    ua, "打包下载");
                            ZMJSON out         = ZMJSON::object();
                            out["token"]       = zm_file_row_str(t, "token");
                            out["expire_time"] = zm_file_row_int(t, "expire_time", 0);
                            out["name"]        = zm_file_row_str(t, "name");
                            co_return out;
                        }
                    }
                }
            }
            else if (st == zm_file::kTaskQueued || st == zm_file::kTaskRunning)
            {
                // 压缩中:幂等返回同一任务号,前端 2s 后重试
                ZMJSON out     = ZMJSON::object();
                out["task_no"] = existTask;
                co_return out;
            }
        }
        // 任务已失败/被清理:重新发起
        std::lock_guard<std::mutex> lk(m_packMtx);
        m_packKeys.erase(key);
    }

    ZmOpCtx ctx;
    ctx.uid        = ownerUid;
    ctx.ip         = ip;
    ZMJSON created = co_await m_pack->Create(space, ids, ctx);
    if (ZmFileHasError(created))
        co_return created;
    std::string taskNo = zm_file_row_str(created, "task_no");
    {
        std::lock_guard<std::mutex> lk(m_packMtx);
        m_packKeys[key] = taskNo;
    }
    if (m_audit)
        co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogPack, 1,
                                            viewerUid, ip, ua, "打包中");
    (void)rootId;
    ZMJSON out     = ZMJSON::object();
    out["task_no"] = taskNo;
    co_return out;
}
