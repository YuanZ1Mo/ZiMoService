#include "modules/module_file_node.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "modules/util/dir_lock.h"
#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <map>
#include <set>

using namespace drogon;

namespace
{
void AddCond(std::string& where, std::vector<std::string>& params, const std::string& cond,
             const std::vector<std::string>& vals)
{
    std::string out;
    size_t      vi = 0;
    for (char c : cond)
    {
        if (c == '?')
        {
            out += "?" + std::to_string(params.size() + vi + 1);
            ++vi;
        }
        else
        {
            out += c;
        }
    }
    where += " AND " + out;
    params.insert(params.end(), vals.begin(), vals.end());
}

/// 文件类型分类的扩展名清单(与前端 FILE_KINDS 保持一致)
const std::vector<std::string>& KindExts(const std::string& kind)
{
    static const std::vector<std::string> kDoc = {"doc", "docx", "pdf", "xls", "xlsx",
                                                  "ppt", "pptx", "md",  "txt"};
    static const std::vector<std::string> kImg = {"jpg",  "jpeg", "png", "gif",
                                                  "webp", "svg",  "bmp"};
    static const std::vector<std::string> kVid = {"mp4", "mov", "mkv", "avi", "webm"};
    static const std::vector<std::string> kAud = {"mp3", "flac", "wav", "ogg", "m4a"};
    static const std::vector<std::string> kZip = {"zip", "7z", "rar", "gz", "tar"};
    static const std::vector<std::string> kEmpty;
    if (kind == "doc")
        return kDoc;
    if (kind == "img")
        return kImg;
    if (kind == "vid")
        return kVid;
    if (kind == "aud")
        return kAud;
    if (kind == "zip")
        return kZip;
    return kEmpty;
}

/// 路径分隔符统一为 '/' 供展示与前端消费
std::string ToSlashPath(const std::string& winPath)
{
    std::string s = winPath;
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/// 去掉首尾空格(名称规范化)
std::string TrimSpaces(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && s[b] == ' ')
        ++b;
    while (e > b && s[e - 1] == ' ')
        --e;
    return s.substr(b, e - b);
}

/// 拼接 WHERE 用的 id 占位符串(参数从 params 现有长度续号)
std::string IdPlaceholders(const std::vector<int64_t>& ids, std::vector<std::string>& params)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i > 0)
            out += ",";
        out += "?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(ids[i]));
    }
    return out;
}
} // namespace

// ============================================================================
// ZmFileNode
// ============================================================================
/**
 * @brief 给列表里本页的目录补上"子树文件字节数"(字段 bytes)
 *
 * 目录在 nodes 里 size 恒为 0,列表只显示 "N 项"、看不出占多大;批量条/删除/打包/移动
 * 复制的"共 X"也需要目录的体积。用**一次**递归 CTE 把本页所有目录一起算完,避免逐目录
 * 查询(一页几十个目录就是几十条查询)。只算本页 —— 前端能选中的也只有本页条目。
 *
 * @param db   数据模块
 * @param list [in,out] 列表行数组(NodeView 产物);目录行补 bytes
 */
void ZmFileNodeModule::FillDirBytes(ZmFileDbModule* db, ZMJSON& list)
{
    std::vector<int64_t> dirIds;
    for (const auto& v : list)
        if (zm_file_row_int(v, "type", 0) == zm_file::kTypeDir)
            dirIds.push_back(zm_file_row_int(v, "id", 0));
    if (dirIds.empty())
        return;
    std::sort(dirIds.begin(), dirIds.end());
    dirIds.erase(std::unique(dirIds.begin(), dirIds.end()), dirIds.end());
    std::string inList = "(";
    for (size_t i = 0; i < dirIds.size(); ++i)
    {
        if (i)
            inList += ",";
        inList += std::to_string(dirIds[i]);
    }
    inList += ")";
    // 每个根目录一棵子树,聚合出该目录下全部文件的字节总和(根目录自身 type=1 不计入 SUM)
    ZMJSON agg = db->QueryRowsSync(
        "WITH RECURSIVE sub(id, size, type, root, depth) AS ("
        " SELECT id, size, type, id, 0 FROM nodes WHERE id IN " + inList +
        "   AND type = 1 AND deleted = 0"
        " UNION ALL"
        " SELECT n.id, n.size, n.type, s.root, s.depth + 1 FROM nodes n"
        " JOIN sub s ON n.parent_id = s.id AND n.deleted = 0 WHERE s.depth < 64)"
        " SELECT root, COALESCE(SUM(CASE WHEN type = 2 THEN size ELSE 0 END), 0) AS bytes"
        " FROM sub GROUP BY root;");
    for (const auto& a : agg)
    {
        int64_t root = zm_file_row_int(a, "root", 0);
        int64_t by   = zm_file_row_int(a, "bytes", 0);
        for (auto& v : list)
        {
            if (zm_file_row_int(v, "id", 0) == root)
            {
                v["bytes"] = by;
                break;
            }
        }
    }
}

ZmFileNode ZmFileNode::FromRow(const ZMJSON& row)
{
    ZmFileNode n;
    n.id             = zm_file_row_int(row, "id", 0);
    n.space          = zm_file_row_int(row, "space", 0);
    n.parentId       = zm_file_row_int(row, "parent_id", 0);
    n.type           = static_cast<int>(zm_file_row_int(row, "type", 0));
    n.name           = zm_file_row_str(row, "name");
    n.size           = zm_file_row_int(row, "size", 0);
    n.ext            = zm_file_row_str(row, "ext");
    n.hash           = zm_file_row_str(row, "hash");
    n.ownerUid       = zm_file_row_int(row, "owner_uid", 0);
    n.items          = zm_file_row_int(row, "items", 0);
    n.createTime     = zm_file_row_int(row, "create_time", 0);
    n.updateTime     = zm_file_row_int(row, "update_time", 0);
    n.deleted        = static_cast<int>(zm_file_row_int(row, "deleted", 0));
    n.deleteTime     = zm_file_row_int(row, "delete_time", 0);
    n.originParentId = zm_file_row_int(row, "origin_parent_id", 0);
    n.delOwnerUid    = zm_file_row_int(row, "del_owner_uid", 0);
    return n;
}

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileNodeModule::ZmFileNodeModule(ZmFileDbModule* db, ZmFileStoreModule* store,
                                   ZmDirLock* lock, ZmFileAuditModule* audit)
    : m_db(db), m_store(store), m_lock(lock), m_audit(audit)
{
}

ZmFileNodeModule::~ZmFileNodeModule() = default;

// ============================================================================
// 空间权限 / 名称校验
// ============================================================================
bool ZmFileNodeModule::SpaceWritable(int64_t space, int64_t uid)
{
    // 个人空间写死:目标空间等于操作者;公共空间任意登录用户可写
    return space == 0 || space == uid;
}

bool ZmFileNodeModule::ValidateName(const std::string& name, std::string& message)
{
    if (name.empty())
    {
        message = "名称不能为空";
        return false;
    }
    if (name.size() > static_cast<size_t>(zm_file::kNameMaxBytes))
    {
        message = "名称过长(最多 255 字节)";
        return false;
    }
    if (name == "." || name == "..")
    {
        message = "名称不能是 . 或 ..";
        return false;
    }
    if (name.back() == ' ' || name.back() == '.')
    {
        message = "名称不能以空格或点结尾";
        return false;
    }
    for (unsigned char c : name)
    {
        if (c < 0x20)
        {
            message = "名称不能包含控制字符";
            return false;
        }
        if (c < 0x80 && (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                         c == '\\' || c == '|' || c == '?' || c == '*'))
        {
            message = "名称不能包含 < > : \" / \\ | ? * 这些字符";
            return false;
        }
    }
    // Windows 保留设备名(不分大小写,含带扩展名的形式)
    std::string base = name;
    size_t      dot  = base.find('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);
    std::string upper;
    upper.reserve(base.size());
    for (char c : base)
        upper += static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    static const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                      "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                      "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                      "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : kReserved)
    {
        if (upper == r)
        {
            message = "该名称是系统保留名,请换一个";
            return false;
        }
    }
    return true;
}

std::string ZmFileNodeModule::DupName(const std::string& name, int index)
{
    std::string suffix = " (" + std::to_string(index) + ")";
    size_t      dot    = name.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return name + suffix;
    return name.substr(0, dot) + suffix + name.substr(dot);
}

std::string ZmFileNodeModule::ExtOf(const std::string& name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
        return "";
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return ext;
}

// ============================================================================
// 可见树判定 / 路径 / 子树
// ============================================================================
bool ZmFileNodeModule::HiddenByAncestorSync(int64_t nodeId)
{
    if (nodeId == 0)
        return false;
    ZMJSON row =
        m_db->QueryRowSync("WITH RECURSIVE up(id, parent_id, deleted, depth) AS ("
                           " SELECT id, parent_id, deleted, 0 FROM nodes WHERE id = ?1"
                           " UNION ALL"
                           " SELECT n.id, n.parent_id, n.deleted, up.depth + 1 FROM nodes n"
                           " JOIN up ON n.id = up.parent_id WHERE up.depth < 64)"
                           " SELECT COUNT(*) AS n FROM up WHERE deleted = 1;",
                           {std::to_string(nodeId)});
    return zm_file_row_int(row, "n", 0) > 0;
}

bool ZmFileNodeModule::VisibleSync(int64_t nodeId, ZMJSON& row)
{
    row = m_db->NodeRowSync(nodeId);
    if (row.empty())
        return false;
    if (zm_file_row_int(row, "deleted", 0) != 0)
        return false;
    return !HiddenByAncestorSync(nodeId);
}

bool ZmFileNodeModule::VisibleSync(int64_t nodeId)
{
    ZMJSON row;
    return VisibleSync(nodeId, row);
}

std::string ZmFileNodeModule::RelPathSync(int64_t nodeId, int64_t* space)
{
    int64_t     sp = 0;
    std::string rel;
    if (!m_db->PathPartsSync(nodeId, sp, rel))
    {
        if (space)
            *space = sp;
        return "";
    }
    if (space)
        *space = sp;
    return ToSlashPath(rel);
}

ZMJSON ZmFileNodeModule::SubtreeIdsSync(int64_t nodeId, int64_t* bytes, int64_t* items)
{
    ZMJSON  rows       = m_db->QueryRowsSync("WITH RECURSIVE sub(id, size, type, depth) AS ("
                                                    " SELECT id, size, type, 0 FROM nodes WHERE id = ?1"
                                                    " UNION ALL"
                                                    " SELECT n.id, n.size, n.type, s.depth + 1 FROM nodes n"
                                                    " JOIN sub s ON n.parent_id = s.id WHERE s.depth < 64)"
                                                    " SELECT id, size, type FROM sub;",
                                             {std::to_string(nodeId)});
    ZMJSON  ids        = ZMJSON::array();
    int64_t totalBytes = 0;
    int64_t count      = 0;
    for (const auto& r : rows)
    {
        ids.push_back(zm_file_row_int(r, "id", 0));
        if (zm_file_row_int(r, "type", 0) == zm_file::kTypeFile)
            totalBytes += zm_file_row_int(r, "size", 0);
        ++count;
    }
    if (bytes)
        *bytes = totalBytes;
    if (items)
        *items = count;
    return ids;
}

int64_t ZmFileNodeModule::SubtreeMaxUpdateSync(const std::vector<int64_t>& ids)
{
    if (ids.empty())
        return 0;
    std::string in;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i)
            in += ",";
        in += std::to_string(ids[i]);
    }
    ZMJSON row = m_db->QueryRowSync(
        "WITH RECURSIVE sub(id, update_time, depth) AS ("
        " SELECT id, update_time, 0 FROM nodes WHERE id IN (" + in + ")"
        " UNION ALL"
        " SELECT n.id, n.update_time, s.depth + 1 FROM nodes n JOIN sub s"
        " ON n.parent_id = s.id WHERE s.depth < 64)"
        " SELECT COALESCE(MAX(update_time), 0) AS t FROM sub;",
        {});
    return zm_file_row_int(row, "t", 0);
}

bool ZmFileNodeModule::QuotaOkSync(int64_t space, int64_t addBytes)
{
    if (space == 0)
        return true;
    ZMJSON row = m_db->SpaceRowSync(space);
    if (row.empty())
        return true;
    int64_t quota = zm_file_row_int(row, "quota", 0);
    if (quota <= 0)
        return true; // 0 = 不限
    int64_t used = zm_file_row_int(row, "used_size", 0);
    return used + addBytes <= quota;
}

// ============================================================================
// 排序 / 筛选 / 视图
// ============================================================================
std::string ZmFileNodeModule::OrderByClause(const ZmListQuery& q)
{
    std::string dir = (q.order == "desc") ? " DESC" : " ASC";
    // 目录恒在前;组内按排序键,末位以名称与 id 兜底保证分页稳定
    if (q.sort == "size")
        return " ORDER BY type ASC, size" + dir + ", name ASC, id ASC";
    if (q.sort == "mtime")
        return " ORDER BY type ASC, update_time" + dir + ", name ASC, id ASC";
    if (q.sort == "type")
        return " ORDER BY type ASC, ext" + dir + ", name ASC, id ASC";
    return " ORDER BY type ASC, name" + dir + ", id ASC";
}

std::string ZmFileNodeModule::TypeFilterClause(const ZmListQuery&        q,
                                               std::vector<std::string>& params)
{
    if (q.typeFilter.empty())
        return "";
    if (q.typeFilter == "dir")
        return " AND type = " + std::to_string(zm_file::kTypeDir);
    std::string list;
    if (q.typeFilter == "oth")
    {
        // 其他 = 文件且扩展名不在已知分类内
        for (const char* kind : {"doc", "img", "vid", "aud", "zip"})
        {
            for (const auto& e : KindExts(kind))
            {
                if (!list.empty())
                    list += ",";
                list += "?" + std::to_string(params.size() + 1);
                params.push_back(e);
            }
        }
        return " AND type = " + std::to_string(zm_file::kTypeFile) + " AND ext NOT IN (" +
               list + ")";
    }
    const std::vector<std::string>& exts = KindExts(q.typeFilter);
    if (exts.empty())
        return "";
    for (const auto& e : exts)
    {
        if (!list.empty())
            list += ",";
        list += "?" + std::to_string(params.size() + 1);
        params.push_back(e);
    }
    return " AND type = " + std::to_string(zm_file::kTypeFile) + " AND ext IN (" + list + ")";
}

ZMJSON ZmFileNodeModule::NodeView(const ZMJSON& row)
{
    ZMJSON j         = ZMJSON::object();
    j["id"]          = zm_file_row_int(row, "id", 0);
    j["type"]        = zm_file_row_int(row, "type", 0);
    j["name"]        = zm_file_row_str(row, "name");
    j["size"]        = zm_file_row_int(row, "size", 0);
    j["ext"]         = zm_file_row_str(row, "ext");
    j["items"]       = zm_file_row_int(row, "items", 0);
    j["owner_uid"]   = zm_file_row_int(row, "owner_uid", 0);
    j["create_time"] = zm_file_row_int(row, "create_time", 0);
    j["update_time"] = zm_file_row_int(row, "update_time", 0);
    return j;
}

// ============================================================================
// 列表
// ============================================================================
ZMJSON ZmFileNodeModule::ListSync(int64_t space, int64_t dirId, const ZmListQuery& q)
{
    if (dirId != 0)
    {
        ZMJSON row;
        if (!VisibleSync(dirId, row) || zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
            zm_file_row_int(row, "space", 0) != space)
            return ZmFileError(zm_file_err::kDirNotFound, 404, "目录不存在");
    }

    std::string              where  = " WHERE space = ?1 AND parent_id = ?2 AND deleted = 0";
    std::vector<std::string> params = {std::to_string(space), std::to_string(dirId)};
    where += TypeFilterClause(q, params);
    if (q.mtimeFrom > 0)
        AddCond(where, params, "update_time >= ?", {std::to_string(q.mtimeFrom)});
    if (q.mtimeTo > 0)
        AddCond(where, params, "update_time <= ?", {std::to_string(q.mtimeTo)});

    ZMJSON  totalRow = m_db->QueryRowSync("SELECT COUNT(*) AS n FROM nodes" + where, params);
    int64_t total    = zm_file_row_int(totalRow, "n", 0);

    std::vector<std::string> lp = params;
    lp.push_back(std::to_string(q.size));
    lp.push_back(std::to_string((q.page - 1) * q.size));
    ZMJSON rows = m_db->QueryRowsSync("SELECT * FROM nodes" + where + OrderByClause(q) +
                                          " LIMIT ?" + std::to_string(lp.size() - 1) +
                                          " OFFSET ?" + std::to_string(lp.size()),
                                      lp);

    ZMJSON list = ZMJSON::array();
    for (const auto& r : rows)
        list.push_back(NodeView(r));
    // 目录补子树字节(需求 §3.1 原定"不实时计算",这里批量一次算完,展示与批量操作共用)
    FillDirBytes(m_db, list);

    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = q.page;
    out["size"]  = q.size;
    out["list"]  = std::move(list);
    ZMJSON bc    = ZMJSON::array();
    if (dirId != 0)
    {
        // 面包屑 = 祖先链 + 当前目录(根目录为 [])
        bc = m_db->QueryRowsSync("WITH RECURSIVE up(id, parent_id, name, depth) AS ("
                                 " SELECT id, parent_id, name, 0 FROM nodes WHERE id = ?1"
                                 " UNION ALL"
                                 " SELECT n.id, n.parent_id, n.name, up.depth + 1 FROM nodes n"
                                 " JOIN up ON n.id = up.parent_id WHERE up.depth < 64)"
                                 " SELECT id, name FROM up ORDER BY depth DESC;",
                                 {std::to_string(dirId)});
    }
    out["breadcrumb"] = std::move(bc);
    out["space"]      = space;
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::List(int64_t space, int64_t dirId, const ZmListQuery& q)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>([this, space, dirId, q]() -> ZMJSON
                                                       { return ListSync(space, dirId, q); });
}

// ============================================================================
// 搜索
// ============================================================================
ZMJSON ZmFileNodeModule::SearchSync(int64_t space, int64_t dirId, const std::string& keyword,
                                    const ZmListQuery& q)
{
    ZMJSON out       = ZMJSON::object();
    out["total"]     = 0;
    out["page"]      = q.page;
    out["size"]      = q.size;
    out["truncated"] = false;
    out["list"]      = ZMJSON::array();

    std::string kw = TrimSpaces(keyword);
    if (kw.empty() || kw.size() > static_cast<size_t>(zm_file::kSearchKwMax))
        return out;

    std::string prefix; // 搜索基准目录相对空间根的路径
    if (dirId != 0)
    {
        ZMJSON row;
        if (!VisibleSync(dirId, row) || zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
            zm_file_row_int(row, "space", 0) != space)
            return ZmFileError(zm_file_err::kDirNotFound, 404, "目录不存在");
        prefix = RelPathSync(dirId);
    }

    // 子树展开同时累积相对路径,避免逐条回查祖先链
    std::string              cte;
    std::vector<std::string> params;
    if (dirId == 0)
    {
        cte =
            "WITH RECURSIVE sub(id, depth, path) AS ("
            " SELECT id, 0, name FROM nodes WHERE space = ?1 AND parent_id = 0 AND deleted = 0"
            " UNION ALL"
            " SELECT n.id, s.depth + 1, s.path || '/' || n.name FROM nodes n JOIN sub s"
            " ON n.parent_id = s.id WHERE n.space = ?1 AND n.deleted = 0 AND s.depth < 64)";
        params.push_back(std::to_string(space));
    }
    else
    {
        cte = "WITH RECURSIVE sub(id, depth, path) AS ("
              " SELECT id, 0, name FROM nodes WHERE id = ?1"
              " UNION ALL"
              " SELECT n.id, s.depth + 1, s.path || '/' || n.name FROM nodes n JOIN sub s"
              " ON n.parent_id = s.id WHERE n.space = ?2 AND n.deleted = 0 AND s.depth < 64)";
        params.push_back(std::to_string(dirId));
        params.push_back(std::to_string(space));
    }

    std::string where = " WHERE 1=1";
    AddCond(where, params, "n.name LIKE ?", {"%" + kw + "%"});
    where += TypeFilterClause(q, params);
    if (q.mtimeFrom > 0)
        AddCond(where, params, "n.update_time >= ?", {std::to_string(q.mtimeFrom)});
    if (q.mtimeTo > 0)
        AddCond(where, params, "n.update_time <= ?", {std::to_string(q.mtimeTo)});

    std::string dir = (q.order == "desc") ? " DESC" : " ASC";
    std::string order;
    if (q.sort == "name")
        order = " ORDER BY s.depth ASC, n.name" + dir;
    else if (q.sort == "size")
        order = " ORDER BY s.depth ASC, n.size" + dir;
    else if (q.sort == "mtime")
        order = " ORDER BY s.depth ASC, n.update_time" + dir;
    else // 默认:所在层级由近及远,同级按修改时间倒序
        order = " ORDER BY s.depth ASC, n.update_time DESC";

    ZMJSON totalRow = m_db->QueryRowSync(
        cte + " SELECT COUNT(*) AS n FROM nodes n JOIN sub s ON n.id = s.id" + where, params);
    int64_t total     = zm_file_row_int(totalRow, "n", 0);
    bool    truncated = total > zm_file::kSearchLimit;
    if (truncated)
        total = zm_file::kSearchLimit;

    int64_t offset = static_cast<int64_t>(q.page - 1) * q.size;
    ZMJSON  list   = ZMJSON::array();
    if (offset < total)
    {
        std::vector<std::string> lp = params;
        lp.push_back(std::to_string(std::min<int64_t>(q.size, total - offset)));
        lp.push_back(std::to_string(offset));
        ZMJSON rows = m_db->QueryRowsSync(
            cte + " SELECT n.*, s.path AS sub_path FROM nodes n JOIN sub s ON n.id = s.id" +
                where + order + " LIMIT ?" + std::to_string(lp.size() - 1) + " OFFSET ?" +
                std::to_string(lp.size()),
            lp);
        for (const auto& r : rows)
        {
            ZMJSON      item = NodeView(r);
            std::string p    = zm_file_row_str(r, "sub_path");
            item["path"]     = prefix.empty() ? p : (prefix + "/" + p);
            list.push_back(std::move(item));
        }
        // 命中本页的目录同样补子树字节(搜索结果也能被勾选后做批量操作)
        FillDirBytes(m_db, list);
    }
    out["total"]     = total;
    out["truncated"] = truncated;
    out["list"]      = std::move(list);
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::Search(int64_t space, int64_t dirId,
                                              const std::string& keyword, const ZmListQuery& q)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, dirId, keyword, q]() -> ZMJSON
        { return SearchSync(space, dirId, keyword, q); });
}

ZMJSON ZmFileNodeModule::SearchInTreeSync(int64_t space, const std::vector<int64_t>& anchorIds,
                                          const std::string&           keyword,
                                          bool                         excludeAnchors,
                                          const ZmListQuery&           q)
{
    ZMJSON out       = ZMJSON::object();
    out["total"]     = 0;
    out["page"]      = q.page;
    out["size"]      = q.size;
    out["truncated"] = false;
    out["list"]      = ZMJSON::array();

    std::string kw = TrimSpaces(keyword);
    if (kw.empty() || kw.size() > static_cast<size_t>(zm_file::kSearchKwMax) || anchorIds.empty())
        return out;

    std::vector<std::string> params = {std::to_string(space)};
    std::string anchors = IdPlaceholders(anchorIds, params);   // 参数自 ?2 起续号

    // 锚点行:depth=0、所在目录为空。向下递归时把**父节点的名称**累进 dirpath ——
    // 于是任一行的 dirpath 恰好是"它所在目录相对所属锚点"的路径:直接挂在锚点下的条目为空,
    // 再深一层是父目录名,以此类推(s.name 取的是 CTE 里父节点那一行的名称)
    std::string cte =
        "WITH RECURSIVE sub(id, root_id, name, depth, dirpath) AS ("
        " SELECT id, id, name, 0, '' FROM nodes"
        " WHERE id IN (" + anchors +
        ") AND space = ?1 AND deleted = 0"
        " UNION ALL"
        " SELECT n.id, s.root_id, n.name, s.depth + 1,"
        " CASE WHEN s.depth = 0 THEN ''"
        " WHEN s.dirpath = '' THEN s.name"
        " ELSE s.dirpath || '/' || s.name END"
        " FROM nodes n JOIN sub s ON n.parent_id = s.id"
        " WHERE n.space = ?1 AND n.deleted = 0 AND s.depth < 64)";

    std::string where = " WHERE 1=1";
    if (excludeAnchors)
        where += " AND s.depth > 0";
    AddCond(where, params, "n.name LIKE ?", {"%" + kw + "%"});
    where += TypeFilterClause(q, params);
    if (q.mtimeFrom > 0)
        AddCond(where, params, "n.update_time >= ?", {std::to_string(q.mtimeFrom)});
    if (q.mtimeTo > 0)
        AddCond(where, params, "n.update_time <= ?", {std::to_string(q.mtimeTo)});

    std::string dir = (q.order == "desc") ? " DESC" : " ASC";
    std::string order;
    if (q.sort == "name")
        order = " ORDER BY s.depth ASC, n.name" + dir;
    else if (q.sort == "size")
        order = " ORDER BY s.depth ASC, n.size" + dir;
    else if (q.sort == "mtime")
        order = " ORDER BY s.depth ASC, n.update_time" + dir;
    else // 默认:所在层级由近及远,同级按修改时间倒序
        order = " ORDER BY s.depth ASC, n.update_time DESC, n.id ASC";

    ZMJSON totalRow = m_db->QueryRowSync(
        cte + " SELECT COUNT(*) AS n FROM nodes n JOIN sub s ON n.id = s.id" + where, params);
    int64_t total     = zm_file_row_int(totalRow, "n", 0);
    bool    truncated = total > zm_file::kSearchLimit;
    if (truncated)
        total = zm_file::kSearchLimit;

    int64_t offset = static_cast<int64_t>(q.page - 1) * q.size;
    ZMJSON  list   = ZMJSON::array();
    if (offset < total)
    {
        std::vector<std::string> lp = params;
        lp.push_back(std::to_string(std::min<int64_t>(q.size, total - offset)));
        lp.push_back(std::to_string(offset));
        ZMJSON rows =
            m_db->QueryRowsSync(cte + " SELECT n.*, s.root_id, s.depth, s.dirpath AS dir_path"
                                      " FROM nodes n JOIN sub s ON n.id = s.id" + where + order +
                                    " LIMIT ?" + std::to_string(lp.size() - 1) + " OFFSET ?" +
                                    std::to_string(lp.size()),
                                lp);
        for (const auto& r : rows)
        {
            ZMJSON item      = NodeView(r);
            item["path"]     = zm_file_row_str(r, "dir_path");
            item["depth"]    = zm_file_row_int(r, "depth", 0);
            item["root_id"]  = zm_file_row_int(r, "root_id", 0);
            list.push_back(std::move(item));
        }
        // 命中本页的目录同样补子树字节(与常规列表同口径)
        FillDirBytes(m_db, list);
    }
    out["total"]     = total;
    out["truncated"] = truncated;
    out["list"]      = std::move(list);
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::SearchInTree(int64_t space,
                                                    const std::vector<int64_t>& anchorIds,
                                                    const std::string&          keyword,
                                                    bool                        excludeAnchors,
                                                    const ZmListQuery&          q)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, anchorIds, keyword, excludeAnchors, q]() -> ZMJSON
        { return SearchInTreeSync(space, anchorIds, keyword, excludeAnchors, q); });
}

// ============================================================================
// 详情 / 统计
// ============================================================================
drogon::Task<ZMJSON> ZmFileNodeModule::Detail(int64_t nodeId, int64_t viewerUid, bool adminAll)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, nodeId, viewerUid, adminAll]() -> ZMJSON
        {
            ZMJSON row;
            if (!VisibleSync(nodeId, row))
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
            // 条目 id 是全库自增的,不校验归属就能被逐个试出来:他人空间的条目与"不存在"同构
            if (!adminAll && !SpaceWritable(zm_file_row_int(row, "space", 0), viewerUid))
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
            ZMJSON item  = NodeView(row);
            item["path"] = RelPathSync(nodeId);
            // 详情弹窗同样给目录带上子树字节(单条按需算,和列表口径一致)
            if (zm_file_row_int(row, "type", 0) == zm_file::kTypeDir)
            {
                int64_t subItems = 0;
                int64_t subBytes = 0;
                SubtreeIdsSync(nodeId, &subBytes, &subItems);
                item["bytes"] = subBytes;
            }
            return item;
        });
}

ZMJSON ZmFileNodeModule::StatExec(const std::vector<int64_t>& ids, int64_t viewerUid,
                                  bool adminAll, ZmTaskHandle* handle)
{
    int64_t bytes   = 0;
    int64_t items   = 0;
    ZMJSON  skipped = ZMJSON::array();
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (handle && handle->Cancelled())
            return ZmFileError(zm_file_err::kBadRequest, 400, "任务已取消");
        // 归属校验与详情同一口径:他人空间的条目只计数、不参与统计,避免规模外泄
        if (!adminAll && !SpaceWritable(m_db->NodeSpaceSync(ids[i]), viewerUid))
        {
            skipped.push_back(ids[i]);
            continue;
        }
        int64_t subBytes = 0;
        ZMJSON  sub      = SubtreeIdsSync(ids[i], &subBytes, nullptr);
        items += static_cast<int64_t>(sub.size());
        bytes += subBytes;
        if (handle)
            handle->Progress(bytes, static_cast<int64_t>(i + 1));
    }
    ZMJSON out     = ZMJSON::object();
    out["items"]   = items;
    out["bytes"]   = bytes;
    out["skipped"] = std::move(skipped);
    return out;
}

// ============================================================================
// 冲突 / 父目录随动 / 深度
// ============================================================================
bool ZmFileNodeModule::ConflictSync(int64_t space, int64_t parentId, const std::string& name,
                                    int64_t excludeId, int64_t& dupId, int& dupType)
{
    ZMJSON row = m_db->QueryRowSync(
        "SELECT id, type FROM nodes WHERE space = ?1 AND parent_id = ?2 AND deleted = 0 "
        "AND name = ?3 COLLATE NOCASE AND id <> ?4 LIMIT 1",
        {std::to_string(space), std::to_string(parentId), name, std::to_string(excludeId)});
    if (row.empty())
        return false;
    dupId   = zm_file_row_int(row, "id", 0);
    dupType = static_cast<int>(zm_file_row_int(row, "type", 0));
    return true;
}

std::string ZmFileNodeModule::FreeNameSync(int64_t space, int64_t parentId,
                                           const std::string& name)
{
    int64_t dupId   = 0;
    int     dupType = 0;
    if (!ConflictSync(space, parentId, name, 0, dupId, dupType))
        return name;
    for (int i = 1; i < 10000; ++i)
    {
        std::string cand = DupName(name, i);
        if (cand.size() <= static_cast<size_t>(zm_file::kNameMaxBytes) &&
            !ConflictSync(space, parentId, cand, 0, dupId, dupType))
            return cand;
    }
    return name;
}

bool ZmFileNodeModule::TouchParentSync(ZmSqliteDb& db, int64_t parentId, int64_t dItems)
{
    if (parentId == 0)
        return true; // 空间根不是一条记录
    return db.ExecSync(
        "UPDATE nodes SET items = MAX(items + ?1, 0), update_time = ?2 "
        "WHERE id = ?3",
        {std::to_string(dItems), std::to_string(ZmSqliteDb::Now()), std::to_string(parentId)});
}

bool ZmFileNodeModule::DepthOkSync(int64_t parentId, int64_t addLevels, std::string& message)
{
    int64_t depth = 0; // 父目录自身所在层级(0 = 空间根)
    if (parentId != 0)
    {
        // 逐级向上找父节点:n.id 对上的是当前行的 parent_id(写成 n.id = n.parent_id
        // 是自比较,递归项恒为空,深度会恒为 1)
        ZMJSON row = m_db->QueryRowSync("WITH RECURSIVE up(id, parent_id, depth) AS ("
                                        " SELECT id, parent_id, 0 FROM nodes WHERE id = ?1"
                                        " UNION ALL"
                                        " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n "
                                        "JOIN up ON n.id = up.parent_id"
                                        " WHERE up.depth < 64)"
                                        " SELECT MAX(depth) AS d FROM up;",
                                        {std::to_string(parentId)});
        depth      = zm_file_row_int(row, "d", 0) + 1;
    }
    if (depth + addLevels > zm_file::kMaxDepth)
    {
        message = "目录层级过深(最多 32 层)";
        return false;
    }
    return true;
}
