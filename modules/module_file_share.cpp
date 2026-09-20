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
#include <cstring>
#include <map>
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

/// 分享根条目集合(ShareRootsSync 产物)→ 可见条目 id(保持绑定顺序)
std::vector<int64_t> VisibleRootIds(const ZMJSON& roots)
{
    std::vector<int64_t> ids;
    for (const auto& r : roots)
        if (zm_json_get_bool(r, "visible", false))
            ids.push_back(zm_file_row_int(r, "id", 0));
    return ids;
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
    // 上界按字符集长度算:硬写常数会越界取到结尾的 NUL,生成出无法输入的提取码
    std::uniform_int_distribution<int> dist(0, static_cast<int>(std::strlen(cs)) - 1);
    std::string                        s;
    for (int i = 0; i < 4; ++i)
        s += cs[dist(gen)];
    return s;
}

/**
 * @brief 校验用户自定义的提取码
 *
 * 随机生成的码是 4 位(字母数字、去掉易混淆字符);自定义沿用**同一长度**,
 * 但字符集放开 —— 用户想用 8888 或四个字母都行。只挡空白与控制字符:
 * 它们既不能在分享页正常输入,也会让"看起来一样"的码验不过。
 *
 * @param pwd 待校验的提取码(调用方已去首尾空白)
 * @return 空串 = 合法;否则是可直接回给用户的说明
 */
static std::string CheckCustomPwd(const std::string& pwd)
{
    if (pwd.size() != 4)
        return "提取码须为 4 个字符";
    for (unsigned char c : pwd)
    {
        if (c < 0x21 || c > 0x7E)
            return "提取码只能用 4 个可见字符(不含空格)";
    }
    return {};
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

ZMJSON ZmFileShareModule::ShareRootsSync(const ZMJSON& share)
{
    int64_t shareId = zm_file_row_int(share, "id", 0);
    std::vector<int64_t> ids;
    ZMJSON rows = m_db->QueryRowsSync(
        "SELECT node_id FROM share_nodes WHERE share_id = ?1 ORDER BY sort, id",
        {std::to_string(shareId)});
    for (const auto& r : rows)
    {
        int64_t id = zm_file_row_int(r, "node_id", 0);
        if (id > 0)
            ids.push_back(id);
    }
    // 改造前创建的分享没有关联行:按 shares.node_id 单条兜底(读路径统一)
    if (ids.empty())
        ids.push_back(zm_file_row_int(share, "node_id", 0));
    ZMJSON out = ZMJSON::array();
    for (int64_t id : ids)
    {
        ZMJSON nrow = m_db->NodeRowSync(id);
        ZMJSON item = nrow.empty() ? ZMJSON::object() : ZmFileNodeModule::NodeView(nrow);
        if (nrow.empty())
        {
            // 条目已被彻底删除:仍保留在集合里(visible=false),让"全部不可用"可判定
            item["id"]   = id;
            item["type"] = zm_file_row_int(share, "node_type", 0);
            item["name"] = zm_file_row_str(share, "name");
        }
        item["visible"] = !nrow.empty() && zm_file_row_int(nrow, "deleted", 0) == 0 &&
                          m_node->VisibleSync(id);
        out.push_back(std::move(item));
    }
    return out;
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
    // 目标在回收站/不存在:分享行状态不变,恢复后自动可用。
    // 多选分享里部分条目不可见只跳过该条,全部不可见才判分享不可用
    if (VisibleRootIds(ShareRootsSync(share)).empty())
    {
        code    = zm_file_err::kShareUnavailable;
        message = "分享内容暂不可用(可能已被删除)";
        return 404;
    }
    return 0;
}

bool ZmFileShareModule::InShareTreeSync(int64_t dirId, const std::vector<int64_t>& roots)
{
    if (dirId == 0)
        return true;
    for (int64_t r : roots)
        if (r == dirId)
            return true;
    if (roots.empty())
        return false;
    std::string              inList;
    std::vector<std::string> params = {std::to_string(dirId)};
    for (size_t i = 0; i < roots.size(); ++i)
    {
        if (i)
            inList += ",";
        inList += "?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(roots[i]));
    }
    // 一次上溯到根:任一分享根出现在祖先链上即视为在分享范围内
    ZMJSON row = m_db->QueryRowSync(
        "WITH RECURSIVE up(id, parent_id, depth) AS ("
        " SELECT id, parent_id, 0 FROM nodes WHERE id = ?1"
        " UNION ALL"
        " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n JOIN up ON n.id = up.parent_id"
        " WHERE up.depth < 64)"
        " SELECT COUNT(*) AS n FROM up WHERE id IN (" + inList + ");",
        params);
    return zm_file_row_int(row, "n", 0) > 0;
}

// ============================================================================
// 创建 / 列表 / 修改 / 取消 / 日志
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Create(const ZmOpCtx& ctx, int64_t space,
                                               const std::vector<int64_t>& nodeIds,
                                               bool pwdEnabled, int64_t expireDays,
                                               int64_t expireTime, int64_t maxDownloads,
                                               bool loginOnly, const std::string& displayName,
                                               const std::string& customPwd)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, space, nodeIds, pwdEnabled, expireDays, expireTime, maxDownloads,
         loginOnly, displayName, customPwd]() -> ZMJSON
        {
            // 自定义提取码:留空 = 随机生成;给了就校验,不合规直接回 400
            std::string customPwdTrim = ZmTrimSpaces(customPwd);
            if (pwdEnabled && !customPwdTrim.empty())
            {
                std::string err = CheckCustomPwd(customPwdTrim);
                if (!err.empty())
                    return ZmFileError(zm_file_err::kBadRequest, 400, err);
            }
            // 去重 + 条数上限(多选分享一条记录绑定多个条目,记录本身仍算 1 条有效分享)
            std::vector<int64_t> ids;
            {
                std::set<int64_t> seen;
                for (int64_t id : nodeIds)
                    if (id > 0 && seen.insert(id).second)
                        ids.push_back(id);
            }
            if (ids.empty())
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
            // 无分享专属上限:选中多少绑多少(文件夹只算 1 条,不含其内部文件)。
            // 仅沿用所有批量操作共用的安全阈值,避免构造超大批量请求
            if (static_cast<int64_t>(ids.size()) > zm_file::kBatchMaxIds)
                return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多分享 2000 个条目");
            // 逐条校验:可见 + 同一空间(跨空间的选中集无法用一条分享表达)
            std::vector<ZMJSON> rows;
            int64_t             realSpace = -1;
            for (int64_t id : ids)
            {
                ZMJSON row;
                if (!m_node->VisibleSync(id, row))
                    return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
                int64_t sp = zm_file_row_int(row, "space", 0);
                if (realSpace < 0)
                    realSpace = sp;
                else if (sp != realSpace)
                    return ZmFileError(zm_file_err::kBadRequest, 400,
                                       "选中的条目必须来自同一空间");
                rows.push_back(std::move(row));
            }
            // 公共空间不提供"仅登录可见"(§3.12.2):按**条目实际所在空间**判定 —— 入参 space
            // 只是客户端的说法(传空即为 0),真实空间以 nodes 为准(见上方 realSpace)。
            // 拿入参判定会把个人空间的分享误判成公共空间,开关怎么点都存不下来
            bool effectiveLoginOnly = (realSpace == 0) ? false : loginOnly;
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
                // 自定义优先,留空才随机生成
                pwdPlain = customPwdTrim.empty() ? GeneratePwd() : customPwdTrim;
                pwdHash  = HashPwd(token, pwdPlain);
            }
            // shares.node_id/node_type/name 保留为主条目(第一条)+ 展示名(列表/兼容旧读路径)
            int64_t     nodeId   = ids[0];
            int64_t     nodeType = zm_file_row_int(rows[0], "type", 0);
            // 展示名:自定义优先,没给(或只给了空白)就沿用主条目名。
            // 真实文件名在 share_nodes.name 里,改展示名不影响列表与下载
            std::string custom = ZmTrimSpaces(displayName);
            std::string name   = custom.empty() ? zm_file_row_str(rows[0], "name") : custom;
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
                             std::to_string(effectiveLoginOnly ? 1 : 0), std::to_string(expire),
                             std::to_string(maxDownloads), std::to_string(now)}))
                        return false;
                    shareId = zm_file_row_int(
                        db.QueryRowTxSync("SELECT last_insert_rowid() AS id", {}), "id", 0);
                    // 单条分享也写一行:读路径统一,不必到处 if 单条
                    for (size_t i = 0; i < ids.size(); ++i)
                    {
                        if (!db.ExecSync(
                                "INSERT INTO share_nodes(share_id,node_id,node_type,name,sort,"
                                "create_time) VALUES(?1,?2,?3,?4,?5,?6)",
                                {std::to_string(shareId), std::to_string(ids[i]),
                                 std::to_string(zm_file_row_int(rows[i], "type", 0)),
                                 zm_file_row_str(rows[i], "name"), std::to_string(i),
                                 std::to_string(now)}))
                            return false;
                    }
                    ZMJSON detail         = ZMJSON::object();
                    detail["token"]       = token;
                    detail["path"]        = m_node->RelPathSync(nodeId);
                    detail["expire_time"] = expire;
                    detail["pwd"]         = pwdEnabled;
                    if (ids.size() > 1)
                    {
                        detail["nodes"] = static_cast<int64_t>(ids.size());
                        std::string joined;
                        for (size_t i = 0; i < ids.size(); ++i)
                            joined += (i ? "," : "") + std::to_string(ids[i]);
                        detail["node_ids"] = joined;
                    }
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
            out["node_count"]  = static_cast<int64_t>(ids.size());
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

drogon::Task<ZMJSON> ZmFileShareModule::List(int64_t uid, int status, int64_t space, int page,
                                             int size, const std::string& keyword)
{
    std::string              where = " WHERE uid = ?1";
    std::vector<std::string> p     = {std::to_string(uid)};
    if (space >= 0)
    {
        where += " AND space = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(space));
    }
    // 关键词匹配展示名;口径统一在 ZmAddKeywordCond
    ZmAddKeywordCond(where, p, keyword, {"name"});
    // 有效状态(status=1)但已过期/达下载上限的行,展示上算"已失效"(见下方 status_name)。
    // 落库要等每日清理,若筛选直接比 status 会出现"有效筛选里混着已失效、已失效筛选里反而没有"
    // 的矛盾,故筛选统一按"有效状态"计算
    int64_t     nowSql = ZmSqliteDb::Now();
    std::string effStatus =
        "(CASE WHEN status = 1 AND ((expire_time > 0 AND expire_time < " + std::to_string(nowSql) +
        ") OR (max_downloads > 0 AND download_count >= max_downloads)) THEN 3 ELSE status END)";
    if (status > 0)
        where += " AND " + effStatus + " = " + std::to_string(status);
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
    // 多选分享的条目数(列表类型列显示"N 项";改造前的旧行无关联行,按 1 计)
    std::unordered_map<int64_t, int64_t> nodeCounts;
    if (!rows.empty())
    {
        std::string              inList;
        std::vector<std::string> cp;
        for (const auto& r : rows)
        {
            if (!inList.empty())
                inList += ",";
            inList += "?" + std::to_string(cp.size() + 1);
            cp.push_back(std::to_string(zm_file_row_int(r, "id", 0)));
        }
        ZMJSON cntRows = co_await m_db->QueryRows(
            "SELECT share_id, COUNT(*) AS n FROM share_nodes WHERE share_id IN (" + inList +
                ") GROUP BY share_id;",
            cp);
        for (const auto& c : cntRows)
            nodeCounts[zm_file_row_int(c, "share_id", 0)] = zm_file_row_int(c, "n", 0);
    }
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
        auto it = nodeCounts.find(zm_file_row_int(r, "id", 0));
        v["node_count"] = it != nodeCounts.end() ? it->second : 1;
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
    if (body.contains("name"))
    {
        // 只改展示名(分享页标题/列表);空串 = 不改,免得误清成无名
        std::string nm = ZmTrimSpaces(zm_file_row_str(body, "name"));
        if (!nm.empty())
            addSet("name", nm);
    }
    if (body.contains("login_only"))
        addSet("login_only", std::to_string(zm_file_row_int(body, "login_only", 0) ? 1 : 0));
    // 提取码开关:与创建同口径,用 pwd_enabled 表达(0 = 关掉提取码,分享变免码访问)
    if (body.contains("pwd_enabled") && zm_file_row_int(body, "pwd_enabled", 1) == 0)
    {
        addSet("pwd_hash", "");   // 空串即"没有提取码",与创建时的表示一致
    }
    else if (zm_json_get_bool(body, "reset_pwd", false))
    {
        newPwd = GeneratePwd();
        addSet("pwd_hash", HashPwd(token, newPwd));
    }
    else if (body.contains("pwd"))
    {
        // 自定义新提取码;留空 = 不改(不是"清空提取码",清空没有对应的界面语义)
        std::string custom = ZmTrimSpaces(zm_file_row_str(body, "pwd"));
        if (!custom.empty())
        {
            std::string err = CheckCustomPwd(custom);
            if (!err.empty())
                co_return ZmFileError(zm_file_err::kBadRequest, 400, err);
            newPwd = custom;
            addSet("pwd_hash", HashPwd(token, custom));
        }
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

drogon::Task<ZMJSON> ZmFileShareModule::Resume(int64_t uid, int64_t shareId, const ZmOpCtx& ctx)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE id = ?1 AND uid = ?2",
                                         {std::to_string(shareId), std::to_string(uid)});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kShareNotFound, 404, "分享不存在");
    if (zm_file_row_int(row, "status", 0) != zm_file::kShareCanceled)
        co_return ZmFileError(zm_file_err::kBadRequest, 400, "只有已取消的分享才能恢复");

    // 恢复即重新生效:过期/达下载上限的无法恢复(它已无意义,只能删除)
    int64_t now  = ZmSqliteDb::Now();
    int64_t exp  = zm_file_row_int(row, "expire_time", 0);
    int64_t maxD = zm_file_row_int(row, "max_downloads", 0);
    int64_t done = zm_file_row_int(row, "download_count", 0);
    if ((exp > 0 && exp < now) || (maxD > 0 && done >= maxD))
        co_return ZmFileError(zm_file_err::kShareExpired, 410,
                              "该分享已过期或达下载上限,无法恢复,可删除");

    // 恢复会重新占用一个有效分享名额,按创建上限校验
    ZMJSON cnt = co_await m_db->QueryRow(
        "SELECT COUNT(*) AS n FROM shares WHERE uid = ?1 AND status = 1",
        {std::to_string(uid)});
    if (zm_file_row_int(cnt, "n", 0) >= zm_file::kShareMaxPerUser)
        co_return ZmFileError(zm_file_err::kTooManyShares, 429,
                              "有效分享数已达上限(200),请先取消或删除部分分享");

    int64_t     space  = zm_file_row_int(row, "space", 0);
    int64_t     nodeId = zm_file_row_int(row, "node_id", 0);
    std::string name   = zm_file_row_str(row, "name");
    bool        ok     = co_await m_db->WithTx(
        [&](ZmSqliteDb& db) -> bool
        {
            if (!db.ExecSync("UPDATE shares SET status = ?1, update_time = ?2 WHERE id = ?3",
                                        {std::to_string(zm_file::kShareActive), std::to_string(now),
                            std::to_string(shareId)}))
                return false;
            ZMJSON detail   = ZMJSON::object();
            detail["token"] = zm_file_row_str(row, "token");
            return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                        zm_file::kActShareResume, space, nodeId, name,
                                                        detail.dump(), ctx.ip, 1);
        });
    if (!ok)
        co_return ZmFileError(zm_file_err::kInternal, 500, "恢复分享失败");
    co_return ZMJSON::object();
}

drogon::Task<ZMJSON> ZmFileShareModule::Purge(int64_t uid, const std::vector<int64_t>& ids,
                                              bool inactiveOnly, const ZmOpCtx& ctx)
{
    // 删除 = 彻底移除记录(任意状态都可删)。inactiveOnly 用于"清空非有效记录",
    // 只清已取消 + 已失效,避免一次误伤还在用的有效分享
    if (!inactiveOnly && ids.empty())
        co_return ZmFileError(zm_file_err::kBadRequest, 400, "未指定要删除的分享");

    std::string              where  = "uid = ?1";
    std::vector<std::string> params = {std::to_string(uid)};
    if (inactiveOnly)
        where += " AND status <> 1";
    if (!inactiveOnly)
    {
        std::string ph;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (i)
                ph += ",";
            ph += "?" + std::to_string(params.size() + 1);
            params.push_back(std::to_string(ids[i]));
        }
        where += " AND id IN (" + ph + ")";
    }

    // 先取待删行:审计明细要 token/名称,且删完就查不到了
    ZMJSON rows = co_await m_db->QueryRows(
        "SELECT id, token, name, space, node_id FROM shares WHERE " + where, params);
    if (rows.empty())
    {
        ZMJSON out     = ZMJSON::object();
        out["purged"]  = 0;
        co_return out;
    }
    bool ok = co_await m_db->WithTx(
        [&](ZmSqliteDb& db) -> bool
        {
            for (const auto& r : rows)
            {
                int64_t     id     = zm_file_row_int(r, "id", 0);
                int64_t     space  = zm_file_row_int(r, "space", 0);
                int64_t     nodeId = zm_file_row_int(r, "node_id", 0);
                std::string name   = zm_file_row_str(r, "name");
                if (!db.ExecSync("DELETE FROM shares WHERE id = ?1 AND uid = ?2",
                                 {std::to_string(id), std::to_string(uid)}))
                    return false;
                // 关联条目随分享一起清掉(无外键,避免残留孤儿行)
                if (!db.ExecSync("DELETE FROM share_nodes WHERE share_id = ?1",
                                 {std::to_string(id)}))
                    return false;
                if (m_audit)
                {
                    ZMJSON detail   = ZMJSON::object();
                    detail["token"] = zm_file_row_str(r, "token");
                    if (!m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                   zm_file::kActSharePurge, space, nodeId, name,
                                                   detail.dump(), ctx.ip, 1))
                        return false;
                }
            }
            return true;
        });
    if (!ok)
        co_return ZmFileError(zm_file_err::kInternal, 500, "删除分享记录失败");
    ZMJSON out    = ZMJSON::object();
    out["purged"] = static_cast<int64_t>(rows.size());
    co_return out;
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
drogon::Task<ZMJSON> ZmFileShareModule::Info(const std::string& token, bool credOk,
                                             int64_t viewerUid, const std::string& ip,
                                             const std::string& ua)
{
    ZMJSON row = co_await m_db->QueryRow("SELECT * FROM shares WHERE token = ?1", {token});
    std::string code;
    std::string message;
    int         status    = 0;
    int64_t     shareId   = 0;
    bool        needLogin = false;
    ZMJSON      out       = ZMJSON::object();
    int64_t     ownerUid  = 0;
    ZMJSON      roots     = ZMJSON::array();   // 可见条目(虚拟根/单文件卡片用)
    int64_t     hidden    = 0;                 // 不可见条目数(在回收站/已彻底删除)
    {
        // 可用性判定与计数在同一段同步逻辑里做,避免中间被改
        ZMJSON result = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, viewerUid, token, &code, &message, &status, &ownerUid, &roots,
             &hidden]() -> ZMJSON
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
                // 部分条目不可见只跳过该条,其余照常(§3.12 多选分享)
                for (const auto& n : ShareRootsSync(row))
                {
                    if (zm_json_get_bool(n, "visible", false))
                        roots.push_back(n);
                    else
                        ++hidden;
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
    // 已带有效凭证(刚输过提取码,或刷新/重开页面时 Cookie 还在有效期内)→ 不必再输一次
    out["need_pwd"]       = !zm_file_row_str(row, "pwd_hash").empty() && !credOk;
    out["expired"]        = false;
    out["expire_time"]    = zm_file_row_int(row, "expire_time", 0);
    out["owner_uid"]      = ownerUid;
    out["login_only"]     = zm_file_row_int(row, "login_only", 0);
    out["need_login"]     = needLogin;
    out["max_downloads"]  = zm_file_row_int(row, "max_downloads", 0);
    out["download_count"] = zm_file_row_int(row, "download_count", 0);
    out["node_id"]        = zm_file_row_int(row, "node_id", 0);
    // 多条目形态:multi 按"绑定条目数 > 1"定形(不受个别条目暂时不可见影响),
    // nodes 只给可见条目,hidden_count 供分享页提示"另有 N 项暂不可用"
    out["multi"]        = static_cast<int64_t>(roots.size() + hidden) > 1;
    out["node_count"]   = static_cast<int64_t>(roots.size() + hidden);
    out["hidden_count"] = hidden;
    out["nodes"]        = std::move(roots);
    co_return out;
}

// ============================================================================
// 公开面:提取码校验
// ============================================================================
drogon::Task<ZMJSON> ZmFileShareModule::Verify(const std::string& token,
                                               const std::string& pwd, const std::string& ip,
                                               const std::string& ua)
{
    // 提取码比对的落点(非 0 = 本次确实比对了,需要在协程侧补一条访问日志;
    // 未设提取码 / 分享不可用 / 冷却拦截等分支没有"校验"可言,不记)
    int64_t logShareId = 0;
    int     logResult  = 0;
    ZMJSON  out        = co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, token, pwd, ip, ua, &logShareId, &logResult]() -> ZMJSON
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

            bool pass  = (HashPwd(token, pwd) == stored);
            logShareId = shareId;
            logResult  = pass ? 1 : 2;   // 1=成功 2=失败(与其它分享日志同一口径)
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
            return out;
        });
    // 记提取码校验(成功/失败都记):只读行为走独立异步写,失败不影响校验结果
    if (logShareId != 0)
        co_await m_audit->RecordShareAccess(logShareId, token, zm_file::kShareLogVerify,
                                            logResult, 0, ip, ua, "");
    co_return out;
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
    std::string          code;
    std::string          message;
    int64_t              shareId = 0;
    int64_t              space   = 0;
    int64_t              rootId  = 0;   // 当前目录所属的分享根(面包屑起点)
    bool                 multi   = false;
    std::vector<int64_t> roots;         // 可见根条目(顺序 = 绑定顺序)
    {
        ZMJSON gate = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, dirId, credOk, viewerUid, &code, &message, &roots, &multi,
             &rootId, &space]() -> ZMJSON
            {
                ZMJSON o      = ZMJSON::object();
                int    status = CheckUsableSync(row, code, message);
                if (status != 0)
                {
                    o["status"] = status;
                    return o;
                }
                // 仅登录可见与分享信息同一道门:只在信息接口拦、列表与下载放行,等于没拦
                // (拿到 token 的人直接打接口就能把内容读走)
                if (zm_file_row_int(row, "login_only", 0) == 1 && viewerUid == 0)
                {
                    code        = "NEED_LOGIN";
                    message     = "该分享需要登录后访问";
                    o["status"] = 401;
                    return o;
                }
                if (!zm_file_row_str(row, "pwd_hash").empty() && !credOk)
                {
                    code        = "NEED_PWD";
                    message     = "请先输入提取码";
                    o["status"] = 403;
                    return o;
                }
                // 多条目分享:顶层为虚拟根;部分条目不可见只跳过该条(§3.12 多选分享)
                ZMJSON nodes = ShareRootsSync(row);
                multi        = nodes.size() > 1;
                roots        = VisibleRootIds(nodes);
                if (!InShareTreeSync(dirId, roots))
                {
                    code        = zm_file_err::kNodeNotFound;
                    message     = "目录不在分享范围内";
                    o["status"] = 404;
                    return o;
                }
                // 当前目录所属的分享根:单根时即该根;多根时找命中的那一个(面包屑从它起算)
                rootId = roots[0];
                if (dirId != 0)
                {
                    for (int64_t r : roots)
                    {
                        if (InShareTreeSync(dirId, {r}))
                        {
                            rootId = r;
                            break;
                        }
                    }
                }
                // 分享行里的 space 是创建时记下的:条目被移动到别的空间后就不作数了。
                // 列目录按条目**当前**所属空间走,否则 List 会因空间不符直接 404
                int64_t listId = (dirId == 0) ? rootId : dirId;
                int64_t curSp  = m_db->NodeSpaceSync(listId);
                space          = curSp >= 0 ? curSp : zm_file_row_int(row, "space", 0);
                o["ok"] = true;
                return o;
            });
        if (!zm_json_get_bool(gate, "ok", false))
            co_return ZmFileError(
                code.c_str(), static_cast<int>(zm_file_row_int(gate, "status", 404)), message);
        shareId = zm_file_row_int(row, "id", 0);
    }

    ViewCtx v;
    v.share   = row;
    v.token   = token;
    v.shareId = shareId;
    v.space   = space;
    v.roots   = roots;
    v.rootId  = rootId;
    v.multi   = multi;

    // 带关键词 = 同一张表上的另一种取法(搜当前目录及子目录),位置改按分享根裁剪
    if (!q.keyword.empty())
        co_return co_await SearchInShare(v, dirId, q, viewerUid, ip, ua);

    // 分享根用 0 表示;其余用实际条目 id 列目录
    int64_t baseId = (dirId == 0) ? rootId : dirId;

    // 多条目分享的顶层(dir_id=0)= 虚拟根:平铺各条目,面包屑只有分享名一级
    if (multi && dirId == 0)
    {
        ZMJSON vlist = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, v, q]() -> ZMJSON
            {
                ZMJSON list = ZMJSON::array();
                if (!v.roots.empty())
                {
                    // 这一层是"若干并列的分享根",常规列表接口(按 parent_id 查一层)覆盖不到,
                    // 排序只能自己写;口径必须与列目录一致,故直接复用同一份排序子句
                    std::vector<std::string> params;
                    std::string              in;
                    for (int64_t id : v.roots)
                    {
                        if (!in.empty())
                            in += ",";
                        in += "?" + std::to_string(params.size() + 1);
                        params.push_back(std::to_string(id));
                    }
                    ZMJSON rows = m_db->QueryRowsSync(
                        "SELECT * FROM nodes WHERE id IN (" + in + ")" +
                            ZmFileNodeModule::OrderByClause(q),
                        params);
                    for (const auto& r : rows)
                        list.push_back(ZmFileNodeModule::NodeView(r));
                }
                // 排好序再按页切(条目上限受批量安全阈值约束,≤2000;切完再补目录字节,省一次全量递归)
                int64_t total  = static_cast<int64_t>(list.size());
                int64_t offset = static_cast<int64_t>(q.page - 1) * q.size;
                ZMJSON  page   = ZMJSON::array();
                for (int64_t i = offset; i < total && i < offset + q.size; ++i)
                    page.push_back(std::move(list[i]));
                // 目录补子树字节(与常规列表同一口径)
                ZmFileNodeModule::FillDirBytes(m_db, page);
                ZMJSON out   = ZMJSON::object();
                out["total"] = total;
                out["page"]  = q.page;
                out["size"]  = q.size;
                out["list"]  = std::move(page);
                return out;
            });
        vlist["breadcrumb"] = co_await BuildBreadcrumb(v, dirId);
        vlist["space"]      = space;
        if (m_audit)
            co_await m_audit->RecordShareAccess(shareId, token, zm_file::kShareLogList, 1,
                                                viewerUid, ip, ua, "");
        co_return vlist;
    }

    ZMJSON list = co_await m_node->List(space, baseId, q);
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
    list["breadcrumb"] = co_await BuildBreadcrumb(v, dirId);
    co_return list;
}

drogon::Task<ZMJSON> ZmFileShareModule::BuildBreadcrumb(const ViewCtx& v, int64_t dirId)
{
    // 多条目分享首级是分享名(虚拟根,id=0):它不在 nodes 里,点它回顶层
    ZMJSON bc = ZMJSON::array();
    if (v.multi)
    {
        ZMJSON vm  = ZMJSON::object();
        vm["id"]   = 0;
        vm["name"] = zm_file_row_str(v.share, "name");
        bc.push_back(std::move(vm));
        if (dirId == 0)
            co_return bc;   // 虚拟根本身就是末级,不该把第一个分享根的名字挂上来
    }
    // 单条目分享的顶层没有虚拟根,dir_id=0 说的就是分享根本身
    int64_t baseId   = (dirId == 0) ? v.rootId : dirId;
    ZMJSON rootRow   = co_await m_db->QueryRow("SELECT id, name FROM nodes WHERE id = ?1",
                                               {std::to_string(v.rootId)});
    ZMJSON rootItem  = ZMJSON::object();
    rootItem["id"]   = zm_file_row_int(rootRow, "id", 0);
    rootItem["name"] = zm_file_row_str(rootRow, "name");
    bc.push_back(std::move(rootItem));
    if (baseId != v.rootId)
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
            if (zm_file_row_int(r, "id", 0) == v.rootId)
                afterRoot = true;
        }
        // 当前目录本身作为最后一级(path 上 depth>=1 已把它排除,需补上;否则
        // 进入子目录后面包屑仍只有根,既显示不出当前位置也回不到这一级)
        ZMJSON curRow  = co_await m_db->QueryRow("SELECT id, name FROM nodes WHERE id = ?1",
                                                 {std::to_string(baseId)});
        ZMJSON curItem = ZMJSON::object();
        curItem["id"]   = zm_file_row_int(curRow, "id", 0);
        curItem["name"] = zm_file_row_str(curRow, "name");
        bc.push_back(std::move(curItem));
    }
    co_return bc;
}

drogon::Task<ZMJSON> ZmFileShareModule::SearchInShare(const ViewCtx& v, int64_t dirId,
                                                      const ZmListQuery& q, int64_t viewerUid,
                                                      const std::string& ip, const std::string& ua)
{
    // 虚拟根底下的搜索:基准是各分享根(它们就挂在虚拟根下,自身也要参与匹配);
    // 其余情况基准是当前目录 —— 它在列表里已经占着一行,不该再作为自己的搜索结果出现
    const bool           topFlat = v.multi && dirId == 0;
    const int64_t        baseId  = (dirId == 0) ? v.rootId : dirId;
    std::vector<int64_t> anchors = topFlat ? v.roots : std::vector<int64_t>{baseId};

    ZMJSON found = co_await m_node->SearchInTree(v.space, anchors, q.keyword, !topFlat, q);
    if (ZmFileHasError(found))
    {
        if (m_audit)
            co_await m_audit->RecordShareAccess(v.shareId, v.token, zm_file::kShareLogList, 2,
                                                viewerUid, ip, ua, "搜索失败");
        co_return found;
    }

    // 位置 = 面包屑拼出的目录链(分享顶层 → 当前目录)+ 每条自己的所在目录
    ZMJSON      bc = co_await BuildBreadcrumb(v, dirId);
    std::string prefix;
    for (const auto& b : bc)
    {
        if (!prefix.empty())
            prefix += "/";
        prefix += zm_file_row_str(b, "name");
    }
    // 虚拟根下每条命中的所属分享根各不相同,位置要各自接上自己的根名
    std::map<int64_t, std::string> rootNames;
    if (topFlat && !v.roots.empty())
    {
        std::vector<std::string> params;
        std::string              in;
        for (int64_t id : v.roots)
        {
            if (!in.empty())
                in += ",";
            in += "?" + std::to_string(params.size() + 1);
            params.push_back(std::to_string(id));
        }
        ZMJSON rows = co_await m_db->QueryRows("SELECT id, name FROM nodes WHERE id IN (" + in + ")",
                                               params);
        for (const auto& r : rows)
            rootNames[zm_file_row_int(r, "id", 0)] = zm_file_row_str(r, "name");
    }
    for (auto& item : found["list"])
    {
        std::string path = prefix;
        if (topFlat && zm_file_row_int(item, "depth", 0) > 0)
            path += "/" + rootNames[zm_file_row_int(item, "root_id", 0)];
        std::string dir = zm_file_row_str(item, "path");
        if (!dir.empty())
            path += "/" + dir;
        item["path"] = path;
        item.erase("depth");     // 只服务于上面拼位置,不对外
        item.erase("root_id");
    }
    found["breadcrumb"] = std::move(bc);
    found["space"]      = v.space;
    if (m_audit)
        co_await m_audit->RecordShareAccess(v.shareId, v.token, zm_file::kShareLogList, 1,
                                            viewerUid, ip, ua, "");
    co_return found;
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
    int64_t     space    = 0;
    int64_t     ownerUid = 0;
    {
        ZMJSON gate = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, row, ids, credOk, viewerUid, &code, &message]() -> ZMJSON
            {
                ZMJSON o      = ZMJSON::object();
                int    status = CheckUsableSync(row, code, message);
                if (status != 0)
                {
                    o["status"] = status;
                    return o;
                }
                // 仅登录可见与分享信息同一道门:只在信息接口拦、列表与下载放行,等于没拦
                // (拿到 token 的人直接打接口就能把内容读走)
                if (zm_file_row_int(row, "login_only", 0) == 1 && viewerUid == 0)
                {
                    code        = "NEED_LOGIN";
                    message     = "该分享需要登录后访问";
                    o["status"] = 401;
                    return o;
                }
                if (!zm_file_row_str(row, "pwd_hash").empty() && !credOk)
                {
                    code        = "NEED_PWD";
                    message     = "请先输入提取码";
                    o["status"] = 403;
                    return o;
                }
                // 任一分享根的子树命中即放行(多选分享的条目分散在多棵子树下)
                std::vector<int64_t> roots = VisibleRootIds(ShareRootsSync(row));
                for (int64_t id : ids)
                {
                    if (!InShareTreeSync(id, roots))
                    {
                        code        = zm_file_err::kNodeNotFound;
                        message     = "条目不在分享范围内";
                        o["status"] = 404;
                        return o;
                    }
                }
                o["ok"] = true;
                // 打包产物落在"条目当前所属空间"的缓存目录:分享行里的空间可能已陈旧
                o["space"] = m_db->NodeSpaceSync(ids[0]);
                return o;
            });
        if (!zm_json_get_bool(gate, "ok", false))
            co_return ZmFileError(
                code.c_str(), static_cast<int>(zm_file_row_int(gate, "status", 404)), message);
        shareId  = zm_file_row_int(row, "id", 0);
        ownerUid = zm_file_row_int(row, "uid", 0);
        int64_t curSp = zm_file_row_int(gate, "space", -1);
        space = curSp >= 0 ? curSp : zm_file_row_int(row, "space", 0);
    }

    // 单文件:直接换取下载令牌
    if (ids.size() == 1)
    {
        // 分享路径的归属已由 InShareTreeSync 校验过:这里不按查看者再判一次
        ZMJSON detail = co_await m_node->Detail(ids[0], 0, true);
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
    ZMJSON out     = ZMJSON::object();
    out["task_no"] = taskNo;
    co_return out;
}
