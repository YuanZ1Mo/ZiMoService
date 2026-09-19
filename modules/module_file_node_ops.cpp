#include "modules/module_file_node.h"

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

// 本文件是 ZmFileNodeModule 的结构变更实现(新建/改名/移动/复制/删除/回收站):
// 与 module_file_node.cpp(查询与校验)同属一个模块,拆开只为单文件规模可控。

namespace
{
/// 批量操作条目上限
constexpr size_t kMaxBatch = static_cast<size_t>(zm_file::kBatchMaxIds);

/// 去重并保持首次出现顺序
std::vector<int64_t> DedupeIds(const std::vector<int64_t>& ids)
{
    std::vector<int64_t> out;
    std::set<int64_t>    seen;
    for (int64_t id : ids)
    {
        if (id <= 0 || seen.count(id))
            continue;
        seen.insert(id);
        out.push_back(id);
    }
    return out;
}

/// 拼接逗号分隔的 ?N 占位符
std::string Placeholders(size_t count, std::vector<std::string>& params,
                         const std::vector<std::string>& values)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out += ",";
        out += "?" + std::to_string(params.size() + 1);
        params.push_back(values[i]);
    }
    return out;
}

/// 路径分隔符统一为 '/' 供展示与审计
std::string ToSlash(const std::string& winPath)
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

/// 小写化(同名比较用)
std::string LowerAscii(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return out;
}

/**
 * @brief 取子树规模(含自身):条目数与文件字节数
 *
 * @param db 数据模块
 * @param nodeId 根条目 id
 * @param items [out] 子树条目数(含自身)
 * @param bytes [out] 子树文件字节数
 */
void SubtreeStat(ZmFileDbModule* db, int64_t nodeId, int64_t& items, int64_t& bytes)
{
    ZMJSON row = db->QueryRowSync("WITH RECURSIVE sub(id, size, type, depth) AS ("
                                  " SELECT id, size, type, 0 FROM nodes WHERE id = ?1"
                                  " UNION ALL"
                                  " SELECT n.id, n.size, n.type, s.depth + 1 FROM nodes n"
                                  " JOIN sub s ON n.parent_id = s.id WHERE s.depth < 64)"
                                  " SELECT COUNT(*) AS n, COALESCE(SUM(CASE WHEN type = 2 "
                                  "THEN size ELSE 0 END),0) AS bytes "
                                  "FROM sub;",
                                  {std::to_string(nodeId)});
    items      = zm_file_row_int(row, "n", 0);
    bytes      = zm_file_row_int(row, "bytes", 0);
}

/**
 * @brief 取子树全部条目行(按层级升序,供复制逐层重建)
 *
 * @param db 数据模块
 * @param nodeId 根条目 id
 * @return 行数组,每行含 depth 字段
 */
ZMJSON SubtreeRows(ZmFileDbModule* db, int64_t nodeId)
{
    return db->QueryRowsSync(
        "WITH RECURSIVE sub(id, depth) AS ("
        " SELECT id, 0 FROM nodes WHERE id = ?1"
        " UNION ALL"
        " SELECT n.id, s.depth + 1 FROM nodes n JOIN sub s ON n.parent_id = s.id"
        " WHERE s.depth < 64)"
        " SELECT n.*, s.depth FROM nodes n JOIN sub s ON n.id = s.id ORDER BY s.depth ASC;",
        {std::to_string(nodeId)});
}

/**
 * @brief 取目录的物理路径(调用方保证空间正确)
 *
 * @param db 数据模块
 * @param store 存储模块
 * @param space 空间
 * @param parentId 目录 id(0 = 空间根)
 * @return 物理路径
 */
std::string DirPhysicalPath(ZmFileDbModule* db, ZmFileStoreModule* store, int64_t space,
                            int64_t parentId)
{
    if (parentId == 0)
        return store->SpaceRoot(space);
    std::string rel;
    int64_t     sp = space;
    if (!db->PathPartsSync(parentId, sp, rel) || rel.empty())
        return store->SpaceRoot(space);
    return store->SpaceRoot(sp) + "\\" + rel;
}
} // namespace

// ============================================================================
// 新建目录
// ============================================================================
ZMJSON ZmFileNodeModule::MkdirSync(int64_t space, int64_t parentId, const std::string& name,
                                   const ZmOpCtx& ctx)
{
    std::string trimmed = TrimSpaces(name);
    std::string msg;
    if (!ValidateName(trimmed, msg))
    {
        m_audit->RecordFailSync(ctx, zm_file::kActMkdir, space, 0, trimmed, zm_file_err::kNameInvalid);
        return ZmFileError(zm_file_err::kNameInvalid, 400, msg);
    }
    if (parentId != 0)
    {
        ZMJSON row;
        if (!VisibleSync(parentId, row) ||
            zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
            zm_file_row_int(row, "space", 0) != space)
            return ZmFileError(zm_file_err::kDirNotFound, 404, "上级目录不存在");
    }
    if (!DepthOkSync(parentId, 1, msg))
        return ZmFileError(zm_file_err::kPathTooDeep, 400, msg);

    std::string parentRel;
    {
        int64_t sp = space;
        m_db->PathPartsSync(parentId, sp, parentRel);
    }
    std::string rel = parentRel.empty() ? trimmed : (parentRel + "\\" + trimmed);
    if (!m_store->ValidateLength(space, rel, msg))
        return ZmFileError(zm_file_err::kPathTooLong, 400, msg);

    ZmDirLock::Guard guard(*m_lock, space, parentId);
    int64_t          dupId   = 0;
    int              dupType = 0;
    if (ConflictSync(space, parentId, trimmed, 0, dupId, dupType))
    {
        m_audit->RecordFailSync(ctx, zm_file::kActMkdir, space, dupId, trimmed, zm_file_err::kNameExists);
        return ZmFileError(zm_file_err::kNameExists, 409, "同名文件或文件夹已存在");
    }

    m_db->EnsureSpaceSync(space);
    m_store->EnsureSpaceRoot(space);
    std::string parentPath = m_store->SpaceRoot(space);
    if (!parentRel.empty())
        parentPath += "\\" + parentRel;
    std::string dirPath = parentPath + "\\" + trimmed;
    // 物理先建、DB 后落:DB 失败时删掉刚建的目录,避免留下"库中无记录"的孤儿
    ZmStoreResult mk = m_store->EnsureDir(dirPath);
    if (!mk.ok)
    {
        m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                return m_audit->RecordFileOpSync(
                    db, ctx.uid, ctx.account, zm_file::kActMkdir, space, 0, trimmed,
                    "{\"path\":\"" + ToSlash(rel) + "\"}", ctx.ip, 2);
            });
        bool tooLong = mk.code == static_cast<int>(ZmErrCode::PathTooLong);
        return ZmFileError(tooLong ? zm_file_err::kPathTooLong : zm_file_err::kInternal,
                           tooLong ? 400 : 500, mk.message);
    }

    int64_t now   = ZmSqliteDb::Now();
    int64_t newId = 0;
    bool    ok    = m_db->WithTxSync(
        [&](ZmSqliteDb& db) -> bool
        {
            if (!db.ExecSync(
                    "INSERT INTO nodes(space,parent_id,type,name,size,ext,hash,owner_uid,"
                          "items,create_time,update_time,deleted,delete_time,origin_parent_id,"
                          "del_owner_uid) VALUES(?1,?2,?3,?4,0,'','',?5,0,?6,?6,0,0,0,0)",
                    {std::to_string(space), std::to_string(parentId),
                     std::to_string(zm_file::kTypeDir), trimmed, std::to_string(ctx.uid),
                     std::to_string(now)}))
                return false;
            newId = zm_file_row_int(db.QueryRowTxSync("SELECT last_insert_rowid() AS id", {}),
                                          "id", 0);
            if (!ZmFileDbModule::ApplyUsageSync(db, space, 0, 1))
                return false;
            if (!TouchParentSync(db, parentId, 1))
                return false;
            return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, zm_file::kActMkdir,
                                                   space, newId, trimmed,
                                                   "{\"path\":\"" + ToSlash(rel) + "\"}", ctx.ip, 1);
        });
    if (!ok)
    {
        m_store->RemoveTreeSync(dirPath);
        return ZmFileError(zm_file_err::kInternal, 500, "创建目录失败");
    }
    ZMJSON out         = ZMJSON::object();
    out["id"]          = newId;
    out["name"]        = trimmed;
    out["create_time"] = now;
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::Mkdir(int64_t space, int64_t parentId,
                                             const std::string& name, const ZmOpCtx& ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, parentId, name, ctx]() -> ZMJSON
        { return MkdirSync(space, parentId, name, ctx); });
}

// ============================================================================
// 按相对路径批量建目录(唯一接受客户端路径字符串的入口)
// ============================================================================
ZMJSON ZmFileNodeModule::EnsureDirsSync(int64_t space, int64_t parentId,
                                        const std::vector<std::string>& paths, bool batch,
                                        const ZmOpCtx& ctx)
{
    if (paths.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未提供目录路径");
    if (paths.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多 2000 条路径");
    if (parentId != 0)
    {
        ZMJSON row;
        if (!VisibleSync(parentId, row) ||
            zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
            zm_file_row_int(row, "space", 0) != space)
            return ZmFileError(zm_file_err::kDirNotFound, 404, "上级目录不存在");
    }

    // 逐段校验:切分 → 名称规则 → 段数 + 当前深度 ≤ 32 → 单条总长 ≤ 1024
    int64_t baseDepth = 0;
    {
        std::string msg;
        (void)msg;
        ZMJSON row;
        if (parentId == 0)
        {
            baseDepth = 0;
        }
        else
        {
            row = m_db->QueryRowSync(
                "WITH RECURSIVE up(id, parent_id, depth) AS ("
                " SELECT id, parent_id, 0 FROM nodes WHERE id = ?1"
                " UNION ALL"
                " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n JOIN up ON"
                " n.id = up.parent_id WHERE up.depth < 64)"
                " SELECT MAX(depth) AS d FROM up;",
                {std::to_string(parentId)});
            baseDepth = zm_file_row_int(row, "d", 0) + 1;
        }
    }

    struct Parsed
    {
        std::vector<std::string> segs;
    };
    std::vector<Parsed> parsed;
    parsed.reserve(paths.size());
    for (const auto& raw : paths)
    {
        if (raw.size() > static_cast<size_t>(zm_file::kEnsurePathMax))
            return ZmFileError(zm_file_err::kBadRequest, 400,
                               "单条相对路径过长(最多 1024 字符)");
        Parsed p;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t      pos = raw.find('/', start);
            std::string seg =
                raw.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
            if (seg.empty() || seg == "." || seg == "..")
                return ZmFileError(zm_file_err::kBadRequest, 400,
                                   "相对路径含空段或 . / ..: " + raw);
            std::string msg;
            if (!ValidateName(seg, msg))
                return ZmFileError(zm_file_err::kNameInvalid, 400, msg);
            p.segs.push_back(seg);
            if (pos == std::string::npos)
                break;
            start = pos + 1;
        }
        if (baseDepth + static_cast<int64_t>(p.segs.size()) > zm_file::kMaxDepth)
            return ZmFileError(zm_file_err::kPathTooDeep, 400, "目录层级过深(最多 32 层)");
        parsed.push_back(std::move(p));
    }
    // 按段数从少到多排序,保证父目录先于子目录建
    std::vector<size_t> order(parsed.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&parsed](size_t a, size_t b)
              { return parsed[a].segs.size() < parsed[b].segs.size(); });

    // 已知目录:(父目录 id, 小写名) → 目录 id
    std::map<std::pair<int64_t, std::string>, int64_t> known;
    auto                                               lower = [](const std::string& s)
    {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return out;
    };

    int64_t              created = 0;
    int64_t              reused  = 0;
    ZMJSON               roots   = ZMJSON::object();
    ZMJSON               pathMap = ZMJSON::object(); ///< 相对路径 → 目录 id
    std::vector<int64_t> lastIds(parsed.size(), parentId); ///< 每条路径的末级目录 id
    for (size_t idx : order)
    {
        const Parsed& p   = parsed[idx];
        int64_t       cur = parentId;
        for (size_t level = 0; level < p.segs.size(); ++level)
        {
            const std::string& seg = p.segs[level];
            auto               key = std::make_pair(cur, lower(seg));
            auto               it  = known.find(key);
            if (it != known.end())
            {
                cur = it->second;
                continue;
            }
            // 已存在同名目录 → 复用;类型不符 → 该条报错
            ZMJSON exist =
                m_db->QueryRowSync("SELECT id, type FROM nodes WHERE space = ?1 AND parent_id "
                                   "= ?2 AND deleted = 0 "
                                   "AND name = ?3 COLLATE NOCASE LIMIT 1",
                                   {std::to_string(space), std::to_string(cur), seg});
            if (!exist.empty())
            {
                if (zm_file_row_int(exist, "type", 0) != zm_file::kTypeDir)
                    return ZmFileError(zm_file_err::kNameExists, 409,
                                       "同名文件已存在,无法建立同名目录: " + seg);
                cur        = zm_file_row_int(exist, "id", 0);
                known[key] = cur;
                ++reused;
                if (level == 0 && !roots.contains(seg))
                    roots[seg] = cur;
                continue;
            }
            ZMJSON made = MkdirSync(space, cur, seg, ctx);
            if (ZmFileHasError(made))
                return made;
            cur        = zm_file_row_int(made, "id", 0);
            known[key] = cur;
            ++created;
            if (level == 0 && !roots.contains(seg))
                roots[seg] = cur;
        }
        lastIds[idx] = cur;
        // 逐段记录映射:调用方据此把每个文件放到它所属的那一级目录
        std::string acc;
        int64_t     segParent = parentId;
        for (const auto& seg : p.segs)
        {
            acc = acc.empty() ? seg : (acc + "/" + seg);
            auto it = known.find({segParent, lower(seg)});
            if (it == known.end())
                break;
            segParent     = it->second;
            pathMap[acc]  = segParent;
        }
    }

    if (!batch)
    {
        // 单条模式:返回末级目录 id 与"是否新建"
        ZMJSON out     = ZMJSON::object();
        out["id"]      = lastIds.empty() ? parentId : lastIds[order.back()];
        out["created"] = created > 0;
        return out;
    }
    ZMJSON out     = ZMJSON::object();
    out["created"] = created;
    out["reused"]  = reused;
    out["roots"]   = std::move(roots);
    out["map"]     = std::move(pathMap); // "a/b" → 目录 id(调用方派发文件上传用)
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::EnsureDirs(int64_t space, int64_t parentId,
                                                  const std::vector<std::string>& paths,
                                                  bool batch, const ZmOpCtx& ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, parentId, paths, batch, ctx]() -> ZMJSON
        { return EnsureDirsSync(space, parentId, paths, batch, ctx); });
}

// ============================================================================
// 重命名
// ============================================================================
ZMJSON ZmFileNodeModule::RenameSync(int64_t nodeId, const std::string& name,
                                    const ZmOpCtx& ctx)
{
    std::string trimmed = TrimSpaces(name);
    std::string msg;
    if (!ValidateName(trimmed, msg))
        return ZmFileError(zm_file_err::kNameInvalid, 400, msg);

    ZMJSON row;
    if (!VisibleSync(nodeId, row))
        return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
    ZmFileNode node = ZmFileNode::FromRow(row);
    if (!SpaceWritable(node.space, ctx.uid))
        return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
    if (node.name == trimmed)
        return ZMJSON::object(); // 同名幂等

    std::string oldPath;
    {
        int64_t sp = node.space;
        if (!m_store->PhysicalPathSync(nodeId, sp, oldPath).ok)
            return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
    }
    std::string parentRel;
    {
        int64_t sp = node.space;
        m_db->PathPartsSync(node.parentId, sp, parentRel);
    }
    std::string newRel = parentRel.empty() ? trimmed : (parentRel + "\\" + trimmed);
    if (!m_store->ValidateLength(node.space, newRel, msg))
        return ZmFileError(zm_file_err::kPathTooLong, 400, msg);

    ZmDirLock::Guard guard(*m_lock, node.space, node.parentId);
    int64_t          dupId   = 0;
    int              dupType = 0;
    if (ConflictSync(node.space, node.parentId, trimmed, nodeId, dupId, dupType))
        return ZmFileError(zm_file_err::kNameExists, 409, "同名文件或文件夹已存在");

    std::string parentPath = m_store->SpaceRoot(node.space);
    if (!parentRel.empty())
        parentPath += "\\" + parentRel;
    std::string newPath = parentPath + "\\" + trimmed;

    // 物理先改名;库落不下去时改回原名
    ZmStoreResult mv = m_store->MovePath(oldPath, newPath);
    if (!mv.ok)
    {
        m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                return m_audit->RecordFileOpSync(
                    db, ctx.uid, ctx.account, zm_file::kActRename, node.space, nodeId,
                    node.name,
                    "{\"from\":\"" + ToSlash(RelPathSync(nodeId)) + "\",\"to\":\"" +
                        ToSlash(newRel) + "\",\"error\":\"" + mv.message + "\"}",
                    ctx.ip, 2);
            });
        bool tooLong = mv.code == static_cast<int>(ZmErrCode::PathTooLong);
        bool locked  = mv.code == static_cast<int>(ZmErrCode::Locked) ||
                      mv.code == static_cast<int>(ZmErrCode::AccessDenied);
        return ZmFileError(tooLong
                               ? zm_file_err::kPathTooLong
                               : (locked ? zm_file_err::kFileLocked : zm_file_err::kInternal),
                           tooLong ? 400 : (locked ? 409 : 500), mv.message);
    }

    int64_t     now    = ZmSqliteDb::Now();
    std::string oldRel = ToSlash(RelPathSync(nodeId));
    bool        ok     = m_db->WithTxSync(
        [&](ZmSqliteDb& db) -> bool
        {
            if (!db.ExecSync("UPDATE nodes SET name = ?1, update_time = ?2 WHERE id = ?3",
                                        {trimmed, std::to_string(now), std::to_string(nodeId)}))
                return false;
            if (!TouchParentSync(db, node.parentId, 0))
                return false;
            return m_audit->RecordFileOpSync(
                db, ctx.uid, ctx.account, zm_file::kActRename, node.space, nodeId, trimmed,
                "{\"from\":\"" + oldRel + "\",\"to\":\"" + ToSlash(newRel) + "\"}", ctx.ip, 1);
        });
    if (!ok)
    {
        ZmStoreResult back = m_store->MovePath(newPath, oldPath);
        if (!back.ok)
            DEFAULT_LOG_ERROR("重命名回滚失败 node={} {}", nodeId, back.message);
        return ZmFileError(zm_file_err::kInternal, 500, "重命名失败");
    }
    return ZMJSON::object();
}

drogon::Task<ZMJSON> ZmFileNodeModule::Rename(int64_t nodeId, const std::string& name,
                                              const ZmOpCtx& ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, nodeId, name, ctx]() -> ZMJSON { return RenameSync(nodeId, name, ctx); });
}

// ============================================================================
// 批量移动
// ============================================================================
ZMJSON ZmFileNodeModule::MoveSync(const std::vector<int64_t>& idsIn, int64_t targetSpace,
                                  int64_t targetDir, const std::string& conflict,
                                  const ZmOpCtx& ctx)
{
    std::vector<int64_t> ids = DedupeIds(idsIn);
    if (ids.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未指定条目");
    if (ids.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多操作 2000 个条目");
    if (!SpaceWritable(targetSpace, ctx.uid))
        return ZmFileError(zm_file_err::kPermDenied, 403, "无权写入目标空间");
    if (conflict != zm_file_conflict::kAsk && conflict != zm_file_conflict::kSkip &&
        conflict != zm_file_conflict::kRename && conflict != zm_file_conflict::kOverwrite)
        return ZmFileError(zm_file_err::kBadRequest, 400, "conflict 取值非法");
    {
        ZMJSON row;
        if (targetDir != 0 && (!VisibleSync(targetDir, row) ||
                               zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
                               zm_file_row_int(row, "space", 0) != targetSpace))
            return ZmFileError(zm_file_err::kDirNotFound, 404, "目标目录不存在");
    }
    for (int64_t id : ids)
    {
        if (id == targetDir)
            return ZmFileError(zm_file_err::kMoveIntoSelf, 400, "目标目录不能是条目自身");
    }

    // 源条目批量加载(不可见的一律不参与,与"不存在"同构)
    std::vector<std::string> srcVals;
    srcVals.reserve(ids.size());
    for (int64_t id : ids)
        srcVals.push_back(std::to_string(id));
    std::vector<std::string> params;
    std::string              inClause = Placeholders(srcVals.size(), params, srcVals);
    ZMJSON                   rows     = m_db->QueryRowsSync(
        "SELECT * FROM nodes WHERE deleted = 0 AND id IN (" + inClause + ")", params);
    std::vector<ZmFileNode> srcs;
    for (const auto& r : rows)
    {
        ZmFileNode n = ZmFileNode::FromRow(r);
        if (HiddenByAncestorSync(n.id))
            continue;
        if (!SpaceWritable(n.space, ctx.uid))
            return ZmFileError(zm_file_err::kPermDenied, 403, "无权操作源空间中的条目");
        srcs.push_back(n);
    }
    if (srcs.empty())
        return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");

    // 折叠:祖先与子孙同时被选中时只移动祖先
    {
        std::vector<std::string> foldVals;
        foldVals.reserve(srcs.size());
        for (const auto& s : srcs)
            foldVals.push_back(std::to_string(s.id));
        std::vector<std::string> wp;
        std::string              ids2     = Placeholders(foldVals.size(), wp, foldVals);
        ZMJSON                   foldRows = m_db->QueryRowsSync(
            "WITH RECURSIVE walk(root, id, parent_id, depth) AS ("
                              " SELECT id, id, parent_id, 0 FROM nodes WHERE id IN (" +
                ids2 +
                ")"
                                  " UNION ALL"
                                  " SELECT w.root, n.id, n.parent_id, w.depth + 1 FROM nodes n JOIN walk w"
                                  " ON n.id = w.parent_id WHERE w.depth < 64)"
                                  " SELECT DISTINCT root FROM walk WHERE depth > 0 AND id IN (" +
                ids2 + ");",
            wp);
        std::set<int64_t> drop;
        for (const auto& r : foldRows)
            drop.insert(zm_file_row_int(r, "root", 0));
        if (!drop.empty())
        {
            std::vector<ZmFileNode> kept;
            for (const auto& s : srcs)
            {
                if (!drop.count(s.id))
                    kept.push_back(s);
            }
            srcs.swap(kept);
        }
    }

    // 环路检测:目标目录不能落在任一待移动目录的子树内
    if (targetDir != 0)
    {
        std::vector<std::string> dirVals;
        std::string              dirCond;
        for (const auto& s : srcs)
        {
            if (s.type != zm_file::kTypeDir)
                continue;
            if (!dirCond.empty())
                dirCond += ",";
            dirCond += "?" + std::to_string(dirVals.size() + 1);
            dirVals.push_back(std::to_string(s.id));
        }
        if (!dirCond.empty())
        {
            std::vector<std::string> p = dirVals;
            p.push_back(std::to_string(targetDir));
            ZMJSON hit = m_db->QueryRowSync(
                "WITH RECURSIVE up(id, parent_id, depth) AS ("
                " SELECT id, parent_id, 0 FROM nodes WHERE id = ?" +
                    std::to_string(p.size()) +
                    " UNION ALL"
                    " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n JOIN up"
                    " ON n.id = up.parent_id WHERE up.depth < 64)"
                    " SELECT COUNT(*) AS n FROM up WHERE id IN (" +
                    dirCond + ");",
                p);
            if (zm_file_row_int(hit, "n", 0) > 0)
                return ZmFileError(zm_file_err::kMoveIntoSelf, 400,
                                   "目标目录不能是自身或其子孙");
        }
    }

    // 配额预检:跨空间时才可能新增目标空间占用
    int64_t addBytes = 0;
    for (const auto& s : srcs)
    {
        if (s.space == targetSpace)
            continue;
        int64_t items = 0;
        int64_t bytes = 0;
        SubtreeStat(m_db, s.id, items, bytes);
        addBytes += bytes;
    }
    if (!QuotaOkSync(targetSpace, addBytes))
        return ZmFileError(zm_file_err::kQuotaExceeded, 403, "目标空间配额不足");

    m_db->EnsureSpaceSync(targetSpace);
    m_store->EnsureSpaceRoot(targetSpace);
    // 加锁:各源父目录 + 目标目录(内部按桶号升序去重)
    std::vector<std::pair<int64_t, int64_t>> keys;
    for (const auto& s : srcs)
        keys.emplace_back(s.space, s.parentId);
    keys.emplace_back(targetSpace, targetDir);
    auto guards = m_lock->LockAll(keys);

    // 冲突裁决(整批):发现任一冲突即整批拒绝,让用户看到完整清单再决定
    ZMJSON conflicts = ZMJSON::array();
    for (const auto& s : srcs)
    {
        if (s.space == targetSpace && s.parentId == targetDir)
            continue; // 原地不动不算冲突
        int64_t dupId   = 0;
        int     dupType = 0;
        if (ConflictSync(targetSpace, targetDir, s.name, 0, dupId, dupType))
        {
            ZMJSON c  = ZMJSON::object();
            c["id"]   = s.id;
            c["name"] = s.name;
            c["why"]  = std::string("目标已存在同名") +
                       (dupType == zm_file::kTypeDir ? "文件夹" : "文件");
            conflicts.push_back(std::move(c));
        }
    }
    if (!conflicts.empty() && conflict == zm_file_conflict::kAsk)
    {
        ZMJSON extra       = ZMJSON::object();
        extra["conflicts"] = std::move(conflicts);
        return ZmFileErrorExtra(zm_file_err::kNameExists, 409, "存在同名冲突,请选择处理策略",
                                extra);
    }

    ZMJSON moved   = ZMJSON::array();
    ZMJSON skipped = ZMJSON::array();
    ZMJSON failed  = ZMJSON::array();
    for (const auto& s : srcs)
    {
        if (s.space == targetSpace && s.parentId == targetDir)
        {
            skipped.push_back(s.id);
            continue;
        }
        int64_t     dupId   = 0;
        int         dupType = 0;
        bool        hasDup  = ConflictSync(targetSpace, targetDir, s.name, 0, dupId, dupType);
        std::string finalName = s.name;
        bool        overwrite = false;
        if (hasDup)
        {
            if (conflict == zm_file_conflict::kSkip)
            {
                skipped.push_back(s.id);
                continue;
            }
            if (conflict == zm_file_conflict::kOverwrite && dupType == zm_file::kTypeFile &&
                s.type == zm_file::kTypeFile)
                overwrite = true;
            else
                finalName = FreeNameSync(targetSpace, targetDir, s.name);
        }

        int64_t     sp = s.space;
        std::string srcPath;
        if (!m_store->PhysicalPathSync(s.id, sp, srcPath).ok)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kNodeNotFound;
            failed.push_back(std::move(f));
            continue;
        }
        std::string targetRel;
        {
            int64_t tsp = targetSpace;
            m_db->PathPartsSync(targetDir, tsp, targetRel);
        }
        std::string newRel = targetRel.empty() ? finalName : (targetRel + "\\" + finalName);
        std::string msg;
        if (!m_store->ValidateLength(targetSpace, newRel, msg))
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kPathTooLong;
            failed.push_back(std::move(f));
            continue;
        }
        m_store->EnsureSpaceRoot(targetSpace);
        std::string dstPath =
            DirPhysicalPath(m_db, m_store, targetSpace, targetDir) + "\\" + finalName;
        std::string fromRel = ToSlash(RelPathSync(s.id));

        // 物理先搬(同盘 MoveFileEx 原子;覆盖由 REPLACE_EXISTING 保证)
        ZmStoreResult mv = m_store->MovePath(srcPath, dstPath);
        if (!mv.ok)
        {
            m_db->WithTxSync(
                [&](ZmSqliteDb& db) -> bool
                {
                    return m_audit->RecordFileOpSync(
                        db, ctx.uid, ctx.account, zm_file::kActMove, s.space, s.id, s.name,
                        "{\"from\":\"" + fromRel + "\",\"to\":\"" + ToSlash(newRel) +
                            "\",\"error\":\"" + mv.message + "\"}",
                        ctx.ip, 2);
                });
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = (mv.code == static_cast<int>(ZmErrCode::Locked) ||
                         mv.code == static_cast<int>(ZmErrCode::AccessDenied))
                            ? zm_file_err::kFileLocked
                            : zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }

        int64_t subItems = 0;
        int64_t subBytes = 0;
        SubtreeStat(m_db, s.id, subItems, subBytes);
        int64_t dupSize = 0;
        if (overwrite)
        {
            ZMJSON dupRow = m_db->NodeRowSync(dupId);
            dupSize       = zm_file_row_int(dupRow, "size", 0);
        }
        int64_t now = ZmSqliteDb::Now();
        bool    ok  = m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                if (overwrite)
                {
                    // 覆盖:目标文件的行与占用一并清除,由源条目顶替
                    // 被顶掉的那份内容已经不存在,指向它的分享同样要失效
                    if (!ZmFileDbModule::InvalidateSharesByNodesSync(db, {dupId}))
                        return false;
                    if (!db.ExecSync("DELETE FROM nodes WHERE id = ?1",
                                         {std::to_string(dupId)}))
                        return false;
                    if (!ZmFileDbModule::ApplyUsageSync(db, targetSpace, -dupSize, -1))
                        return false;
                }
                if (!db.ExecSync("UPDATE nodes SET space = ?1, parent_id = ?2, name = ?3, "
                                         "update_time = ?4 WHERE id = ?5",
                                     {std::to_string(targetSpace), std::to_string(targetDir),
                                  finalName, std::to_string(now), std::to_string(s.id)}))
                    return false;
                // 子孙只改 space:它们跟着父目录走,自身父子关系没变
                if (s.space != targetSpace)
                {
                    if (!db.ExecSync("WITH RECURSIVE sub(id, depth) AS ("
                                             " SELECT id, 0 FROM nodes WHERE id = ?1"
                                             " UNION ALL"
                                             " SELECT n.id, s.depth + 1 FROM nodes n JOIN sub s"
                                             " ON n.parent_id = s.id WHERE s.depth < 64)"
                                             " UPDATE nodes SET space = ?2 WHERE id IN "
                                             "(SELECT id FROM sub WHERE depth > 0);",
                                         {std::to_string(s.id), std::to_string(targetSpace)}))
                        return false;
                }
                if (!ZmFileDbModule::ApplyUsageSync(db, s.space, -subBytes, -subItems))
                    return false;
                if (!ZmFileDbModule::ApplyUsageSync(db, targetSpace, subBytes, subItems))
                    return false;
                if (!TouchParentSync(db, s.parentId, -1))
                    return false;
                if (!TouchParentSync(db, targetDir, 1))
                    return false;
                ZMJSON detail  = ZMJSON::object();
                detail["from"] = fromRel;
                detail["to"]   = ToSlash(newRel);
                if (overwrite)
                    detail["overwrite"] = true;
                return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, zm_file::kActMove,
                                                     targetSpace, s.id, finalName, detail.dump(),
                                                     ctx.ip, 1);
            });
        if (!ok)
        {
            // 库没改成:把物理文件搬回原位;搬不回去交给一致性同步修复
            ZmStoreResult back = m_store->MovePath(dstPath, srcPath);
            if (!back.ok)
                DEFAULT_LOG_ERROR("移动回滚失败 node={} {}", s.id, back.message);
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }
        moved.push_back(s.id);
    }

    ZMJSON out     = ZMJSON::object();
    out["moved"]   = std::move(moved);
    out["skipped"] = std::move(skipped);
    if (!failed.empty())
        out["failed"] = std::move(failed);
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::Move(const std::vector<int64_t>& ids,
                                            int64_t targetSpace, int64_t targetDir,
                                            const std::string& conflict, const ZmOpCtx& ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ids, targetSpace, targetDir, conflict, ctx]() -> ZMJSON
        { return MoveSync(ids, targetSpace, targetDir, conflict, ctx); });
}

// ============================================================================
// 复制
// ============================================================================
ZMJSON ZmFileNodeModule::CopySync(const std::vector<int64_t>& idsIn, int64_t targetSpace,
                                  int64_t targetDir, const std::string& conflict,
                                  const ZmOpCtx& ctx, ZmTaskHandle* handle)
{
    std::vector<int64_t> ids = DedupeIds(idsIn);
    if (ids.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未指定条目");
    if (ids.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多复制 2000 个条目");
    if (!SpaceWritable(targetSpace, ctx.uid))
        return ZmFileError(zm_file_err::kPermDenied, 403, "无权写入目标空间");
    if (conflict != zm_file_conflict::kAsk && conflict != zm_file_conflict::kSkip &&
        conflict != zm_file_conflict::kRename && conflict != zm_file_conflict::kOverwrite)
        return ZmFileError(zm_file_err::kBadRequest, 400, "conflict 取值非法");
    {
        ZMJSON row;
        if (targetDir != 0 && (!VisibleSync(targetDir, row) ||
                               zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
                               zm_file_row_int(row, "space", 0) != targetSpace))
            return ZmFileError(zm_file_err::kDirNotFound, 404, "目标目录不存在");
    }
    for (int64_t id : ids)
    {
        if (id == targetDir)
            return ZmFileError(zm_file_err::kMoveIntoSelf, 400, "目标目录不能是条目自身");
    }

    std::vector<std::string> srcVals;
    srcVals.reserve(ids.size());
    for (int64_t id : ids)
        srcVals.push_back(std::to_string(id));
    std::vector<std::string> params;
    std::string              inClause = Placeholders(srcVals.size(), params, srcVals);
    ZMJSON                   rows     = m_db->QueryRowsSync(
        "SELECT * FROM nodes WHERE deleted = 0 AND id IN (" + inClause + ")", params);
    std::vector<ZmFileNode> srcs;
    for (const auto& r : rows)
    {
        ZmFileNode n = ZmFileNode::FromRow(r);
        if (HiddenByAncestorSync(n.id))
            continue;
        if (!SpaceWritable(n.space, ctx.uid))
            return ZmFileError(zm_file_err::kPermDenied, 403, "无权读取源空间中的条目");
        srcs.push_back(n);
    }
    if (srcs.empty())
        return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");

    // 折叠:祖先与子孙同时被复制时只复制祖先(否则子孙会被复制两遍)
    {
        std::vector<std::string> foldVals;
        foldVals.reserve(srcs.size());
        for (const auto& s : srcs)
            foldVals.push_back(std::to_string(s.id));
        std::vector<std::string> wp;
        std::string              ids2     = Placeholders(foldVals.size(), wp, foldVals);
        ZMJSON                   foldRows = m_db->QueryRowsSync(
            "WITH RECURSIVE walk(root, id, parent_id, depth) AS ("
                              " SELECT id, id, parent_id, 0 FROM nodes WHERE id IN (" +
                ids2 +
                ")"
                                  " UNION ALL"
                                  " SELECT w.root, n.id, n.parent_id, w.depth + 1 FROM nodes n JOIN walk w"
                                  " ON n.id = w.parent_id WHERE w.depth < 64)"
                                  " SELECT DISTINCT root FROM walk WHERE depth > 0 AND id IN (" +
                ids2 + ");",
            wp);
        std::set<int64_t> drop;
        for (const auto& r : foldRows)
            drop.insert(zm_file_row_int(r, "root", 0));
        if (!drop.empty())
        {
            std::vector<ZmFileNode> kept;
            for (const auto& s : srcs)
            {
                if (!drop.count(s.id))
                    kept.push_back(s);
            }
            srcs.swap(kept);
        }
    }

    // 环路检测:复制到自身子孙会无限展开
    if (targetDir != 0)
    {
        std::vector<std::string> dirVals;
        std::string              dirCond;
        for (const auto& s : srcs)
        {
            if (s.type != zm_file::kTypeDir)
                continue;
            if (!dirCond.empty())
                dirCond += ",";
            dirCond += "?" + std::to_string(dirVals.size() + 1);
            dirVals.push_back(std::to_string(s.id));
        }
        if (!dirCond.empty())
        {
            std::vector<std::string> p = dirVals;
            p.push_back(std::to_string(targetDir));
            ZMJSON hit = m_db->QueryRowSync(
                "WITH RECURSIVE up(id, parent_id, depth) AS ("
                " SELECT id, parent_id, 0 FROM nodes WHERE id = ?" +
                    std::to_string(p.size()) +
                    " UNION ALL"
                    " SELECT n.id, n.parent_id, up.depth + 1 FROM nodes n JOIN up"
                    " ON n.id = up.parent_id WHERE up.depth < 64)"
                    " SELECT COUNT(*) AS n FROM up WHERE id IN (" +
                    dirCond + ");",
                p);
            if (zm_file_row_int(hit, "n", 0) > 0)
                return ZmFileError(zm_file_err::kMoveIntoSelf, 400,
                                   "目标目录不能是自身或其子孙");
        }
    }

    int64_t addBytes = 0;
    for (const auto& s : srcs)
    {
        int64_t items = 0;
        int64_t bytes = 0;
        SubtreeStat(m_db, s.id, items, bytes);
        addBytes += bytes;
    }
    if (!QuotaOkSync(targetSpace, addBytes))
        return ZmFileError(zm_file_err::kQuotaExceeded, 403, "目标空间配额不足");

    m_db->EnsureSpaceSync(targetSpace);
    m_store->EnsureSpaceRoot(targetSpace);
    std::vector<std::pair<int64_t, int64_t>> keys;
    for (const auto& s : srcs)
        keys.emplace_back(s.space, s.parentId);
    keys.emplace_back(targetSpace, targetDir);
    auto guards = m_lock->LockAll(keys);

    // 冲突裁决(整批):复制到原位同样构成冲突(目标已存在同名),与其它条目一视同仁
    ZMJSON conflicts = ZMJSON::array();
    for (const auto& s : srcs)
    {
        int64_t dupId   = 0;
        int     dupType = 0;
        if (ConflictSync(targetSpace, targetDir, s.name, 0, dupId, dupType))
        {
            ZMJSON c  = ZMJSON::object();
            c["id"]   = s.id;
            c["name"] = s.name;
            c["why"]  = std::string("目标已存在同名") +
                       (dupType == zm_file::kTypeDir ? "文件夹" : "文件");
            conflicts.push_back(std::move(c));
        }
    }
    if (!conflicts.empty() && conflict == zm_file_conflict::kAsk)
    {
        ZMJSON extra       = ZMJSON::object();
        extra["conflicts"] = std::move(conflicts);
        return ZmFileErrorExtra(zm_file_err::kNameExists, 409, "存在同名冲突,请选择处理策略",
                                extra);
    }

    ZMJSON  copied    = ZMJSON::array();
    ZMJSON  skipped   = ZMJSON::array();
    ZMJSON  failed    = ZMJSON::array();
    int64_t doneBytes = 0;
    int64_t doneItems = 0;
    for (const auto& s : srcs)
    {
        if (handle && handle->Cancelled())
            break;
        int64_t     dupId   = 0;
        int         dupType = 0;
        bool        hasDup  = ConflictSync(targetSpace, targetDir, s.name, 0, dupId, dupType);
        std::string finalName = s.name;
        if (hasDup)
        {
            if (conflict == zm_file_conflict::kSkip)
            {
                skipped.push_back(s.id);
                continue;
            }
            if (conflict == zm_file_conflict::kAsk)
            {
                // ask 已在整批裁决阶段拦下;走到这里说明裁决后又出现冲突(并发改名),
                // 按 skip 处理最安全:不覆盖别人的条目
                skipped.push_back(s.id);
                continue;
            }
            if (conflict == zm_file_conflict::kOverwrite && dupType == zm_file::kTypeFile &&
                s.type == zm_file::kTypeFile)
            {
                finalName = s.name; // 覆盖:按新内容替换同名文件
            }
            else
            {
                // rename 策略;或覆盖但目标是目录/目录对目录 → 按 rename(不做目录合并)
                finalName = FreeNameSync(targetSpace, targetDir, s.name);
            }
        }

        int64_t     sp = s.space;
        std::string srcPath;
        if (!m_store->PhysicalPathSync(s.id, sp, srcPath).ok)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kNodeNotFound;
            failed.push_back(std::move(f));
            continue;
        }
        std::string targetRel;
        {
            int64_t tsp = targetSpace;
            m_db->PathPartsSync(targetDir, tsp, targetRel);
        }
        std::string newRel = targetRel.empty() ? finalName : (targetRel + "\\" + finalName);
        std::string msg;
        if (!m_store->ValidateLength(targetSpace, newRel, msg))
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kPathTooLong;
            failed.push_back(std::move(f));
            continue;
        }
        m_store->EnsureSpaceRoot(targetSpace);
        std::string dstPath =
            DirPhysicalPath(m_db, m_store, targetSpace, targetDir) + "\\" + finalName;

        // 物理先复制(空目录同样被复制,不丢层级)
        ZmStoreResult cp = m_store->CopyTreeSync(srcPath, dstPath);
        if (!cp.ok)
        {
            m_db->WithTxSync(
                [&](ZmSqliteDb& db) -> bool
                {
                    return m_audit->RecordFileOpSync(
                        db, ctx.uid, ctx.account, zm_file::kActCopy, targetSpace, s.id, s.name,
                        "{\"from\":\"" + ToSlash(RelPathSync(s.id)) + "\",\"to\":\"" +
                            ToSlash(newRel) + "\",\"error\":\"" + cp.message + "\"}",
                        ctx.ip, 2);
                });
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = (cp.code == static_cast<int>(ZmErrCode::DiskFull))
                            ? zm_file_err::kInternal
                            : zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }

        int64_t subItems = 0;
        int64_t subBytes = 0;
        SubtreeStat(m_db, s.id, subItems, subBytes);
        int64_t overwriteSize = 0;
        if (hasDup && finalName == s.name && conflict == zm_file_conflict::kOverwrite)
        {
            ZMJSON dupRow = m_db->NodeRowSync(dupId);
            overwriteSize = zm_file_row_int(dupRow, "size", 0);
        }
        int64_t     now       = ZmSqliteDb::Now();
        std::string fromRel   = ToSlash(RelPathSync(s.id));
        int64_t     newRootId = 0;
        ZMJSON      subRows   = SubtreeRows(m_db, s.id);
        bool        ok        = m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                std::map<int64_t, int64_t> idMap;
                for (const auto& r : subRows)
                {
                    int64_t oldId     = zm_file_row_int(r, "id", 0);
                    int64_t oldParent = zm_file_row_int(r, "parent_id", 0);
                    bool    isRoot    = (oldId == s.id);
                    int64_t newParent = targetDir;
                    if (!isRoot)
                    {
                        auto it = idMap.find(oldParent);
                        if (it == idMap.end())
                            return false; // 层级顺序被破坏(不该发生)
                        newParent = it->second;
                    }
                    std::string rowName = isRoot ? finalName : zm_file_row_str(r, "name");
                    if (!db.ExecSync("INSERT INTO "
                                                                 "nodes(space,parent_id,type,name,size,ext,hash,owner_uid,"
                                                                 "items,create_time,update_time,deleted,delete_time,"
                                                                 "origin_parent_id,"
                                                                 "del_owner_uid) "
                                                                 "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?10,0,0,0,0)",
                                                   {std::to_string(targetSpace), std::to_string(newParent),
                                      std::to_string(zm_file_row_int(r, "type", 0)), rowName,
                                      std::to_string(zm_file_row_int(r, "size", 0)),
                                      zm_file_row_str(r, "ext"), zm_file_row_str(r, "hash"),
                                      std::to_string(ctx.uid),
                                      std::to_string(zm_file_row_int(r, "items", 0)),
                                      std::to_string(now)}))
                        return false;
                    int64_t newId = zm_file_row_int(
                        db.QueryRowTxSync("SELECT last_insert_rowid() AS id", {}), "id", 0);
                    idMap[oldId] = newId;
                    if (isRoot)
                        newRootId = newId;
                }
                if (overwriteSize > 0)
                {
                    // 覆盖:目标文件的行与占用一并清除,由新副本顶替
                    if (!db.ExecSync("DELETE FROM nodes WHERE id = ?1",
                                                   {std::to_string(dupId)}))
                        return false;
                    if (!ZmFileDbModule::ApplyUsageSync(db, targetSpace, -overwriteSize, -1))
                        return false;
                    subItems -= 1;
                }
                if (!ZmFileDbModule::ApplyUsageSync(db, targetSpace, subBytes, subItems))
                    return false;
                if (!TouchParentSync(db, targetDir, 1))
                    return false;
                ZMJSON detail   = ZMJSON::object();
                detail["from"]  = fromRel;
                detail["to"]    = ToSlash(newRel);
                detail["items"] = subItems;
                detail["bytes"] = subBytes;
                return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, zm_file::kActCopy,
                                                               targetSpace, newRootId, finalName,
                                                               detail.dump(), ctx.ip, 1);
            });
        if (!ok)
        {
            // 库没落成:把刚复制出来的物理副本清掉
            ZmStoreResult back = m_store->RemoveTreeSync(dstPath);
            if (!back.ok)
                DEFAULT_LOG_ERROR("复制回滚失败 node={} {}", s.id, back.message);
            ZMJSON f  = ZMJSON::object();
            f["id"]   = s.id;
            f["name"] = s.name;
            f["code"] = zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }
        copied.push_back(s.id);
        doneItems += subItems;
        doneBytes += subBytes;
        if (handle)
            handle->Progress(doneBytes, doneItems);
    }

    ZMJSON out     = ZMJSON::object();
    out["copied"]  = std::move(copied);
    out["skipped"] = std::move(skipped);
    out["failed"]  = std::move(failed);
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::Copy(const std::vector<int64_t>& ids,
                                            int64_t targetSpace, int64_t targetDir,
                                            const std::string& conflict, const ZmOpCtx& ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ids, targetSpace, targetDir, conflict, ctx]() -> ZMJSON
        { return CopySync(ids, targetSpace, targetDir, conflict, ctx, nullptr); });
}

ZMJSON ZmFileNodeModule::CopyExec(const std::vector<int64_t>& ids, int64_t targetSpace,
                                  int64_t targetDir, const std::string& conflict,
                                  const ZmOpCtx& ctx, ZmTaskHandle* handle)
{
    return CopySync(ids, targetSpace, targetDir, conflict, ctx, handle);
}

drogon::Task<ZMJSON> ZmFileNodeModule::EstimateCopy(const std::vector<int64_t>& ids)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ids]() -> ZMJSON
        {
            int64_t items = 0;
            int64_t bytes = 0;
            for (int64_t id : DedupeIds(ids))
            {
                int64_t subItems = 0;
                int64_t subBytes = 0;
                SubtreeStat(m_db, id, subItems, subBytes);
                items += subItems;
                bytes += subBytes;
            }
            ZMJSON out   = ZMJSON::object();
            out["items"] = items;
            out["bytes"] = bytes;
            return out;
        });
}

// ============================================================================
// 软删除
// ============================================================================
ZMJSON ZmFileNodeModule::SoftDeleteSync(const std::vector<int64_t>& idsIn, const ZmOpCtx& ctx)
{
    std::vector<int64_t> ids = DedupeIds(idsIn);
    if (ids.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未指定条目");
    if (ids.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多删除 2000 个条目");

    std::vector<std::string> srcVals;
    srcVals.reserve(ids.size());
    for (int64_t id : ids)
        srcVals.push_back(std::to_string(id));
    std::vector<std::string> params;
    std::string              inClause = Placeholders(srcVals.size(), params, srcVals);
    ZMJSON                   rows     = m_db->QueryRowsSync(
        "SELECT * FROM nodes WHERE deleted = 0 AND id IN (" + inClause + ")", params);

    ZMJSON                  success    = ZMJSON::array();
    ZMJSON                  failed     = ZMJSON::array();
    ZMJSON                  auditItems = ZMJSON::array();
    std::vector<ZmFileNode> nodes;
    for (const auto& r : rows)
    {
        ZmFileNode n = ZmFileNode::FromRow(r);
        if (HiddenByAncestorSync(n.id) || !SpaceWritable(n.space, ctx.uid))
            continue;
        nodes.push_back(n);
    }
    // 锁各源父目录,与新建/改名/移动串行化
    std::vector<std::pair<int64_t, int64_t>> keys;
    for (const auto& n : nodes)
        keys.emplace_back(n.space, n.parentId);
    auto guards = m_lock->LockAll(keys);

    // 物理侧先行:把条目从在用目录搬进回收站,再改库。文件动作进不了数据库事务,
    // 故取"先搬后记":搬不动、算不出源路径的条目本轮不删(库行原样保留,下轮重试),
    // 免得留下"行已进回收站、文件却还在在用目录"这种半截状态
    std::vector<ZmFileNode> moved;      // 本轮可进回收站的条目(物理侧已就绪)
    std::vector<ZmFileNode> movedPhys;  // 其中真的搬动了文件的(回滚时按它搬回)
    for (const auto& n : nodes)
    {
        auto fail = [&](const char* code)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = n.id;
            f["name"] = n.name;
            f["code"] = code;
            failed.push_back(std::move(f));
        };
        int64_t     sp = n.space;
        std::string src;
        if (!m_store->PhysicalPathSync(n.id, sp, src).ok)
        {
            fail(zm_file_err::kNodeNotFound);
            continue;
        }
        if (!m_store->EnsureDir(m_store->TrashEntryDir(n.space, n.id)).ok)
        {
            fail(zm_file_err::kInternal);
            continue;
        }
        if (m_store->Exists(src))
        {
            ZmStoreResult mv =
                m_store->MovePath(src, m_store->TrashEntryPath(n.space, n.id, n.name));
            if (!mv.ok)
            {
                fail((mv.code == static_cast<int>(ZmErrCode::Locked) ||
                      mv.code == static_cast<int>(ZmErrCode::AccessDenied))
                         ? zm_file_err::kFileLocked
                         : zm_file_err::kInternal);
                continue;
            }
            movedPhys.push_back(n);
        }
        else
        {
            // 源文件本就不在:照常进回收站(条目无内容),交给一致性同步兜底
            DEFAULT_LOG_WARN("ZmFileNodeModule: 删除时源文件已缺失,回收站条目无内容: {}",
                             n.name);
        }
        moved.push_back(n);
    }

    int64_t now = ZmSqliteDb::Now();
    bool    ok  = m_db->WithTxSync(
        [&](ZmSqliteDb& db) -> bool
        {
            for (const auto& n : moved)
            {
                // deleted=0 守卫不可省:重复删除会覆盖原位置与归属
                if (!db.ExecSync("UPDATE nodes SET deleted = 1, delete_time = ?1, "
                                         "origin_parent_id = ?2, del_owner_uid = ?3 "
                                         "WHERE id = ?4 AND deleted = 0",
                                     {std::to_string(now), std::to_string(n.parentId),
                                  std::to_string(ctx.uid), std::to_string(n.id)}))
                    return false;
                int64_t changed =
                    zm_file_row_int(db.QueryRowTxSync("SELECT changes() AS n", {}), "n", 0);
                if (changed == 0)
                {
                    // 库行没动,但文件已被搬进回收站:搬回原位,两者保持一致
                    int64_t     sp = n.space;
                    std::string back;
                    if (m_store->PhysicalPathSync(n.id, sp, back).ok &&
                        m_store->MovePath(m_store->TrashEntryPath(n.space, n.id, n.name), back).ok)
                        m_store->RemoveTreeSync(m_store->TrashEntryDir(n.space, n.id));
                    ZMJSON f  = ZMJSON::object();
                    f["id"]   = n.id;
                    f["name"] = n.name;
                    f["code"] = zm_file_err::kNodeNotFound;
                    failed.push_back(std::move(f));
                    continue;
                }
                if (!TouchParentSync(db, n.parentId, -1))
                    return false;
                success.push_back(n.id);
                ZMJSON item  = ZMJSON::object();
                item["id"]   = n.id;
                item["name"] = n.name;
                item["type"] = n.type;
                item["size"] = n.size;
                item["path"] = ToSlash(RelPathSync(n.id));
                // 目录额外记子树规模:彻底删除的耗时/占用预估要靠它(需求 §3.7.1)
                if (n.type == zm_file::kTypeDir)
                {
                    int64_t subItems = 0;
                    int64_t subBytes = 0;
                    SubtreeStat(m_db, n.id, subItems, subBytes);
                    item["items"] = subItems;
                    item["bytes"] = subBytes;
                }
                auditItems.push_back(std::move(item));
            }
            // 删除一批一条(与「有操作必有记录」一致);回收站占配额,用量不动
            if (!auditItems.empty())
            {
                if (!m_audit->RecordBatchSync(db, ctx.uid, ctx.account, zm_file::kActDelete,
                                              moved.empty() ? 0 : moved.front().space,
                                                  auditItems, ctx.ip, 1))
                    return false;
            }
            return true;
        });
    if (!ok)
    {
        // 事务整体回滚:库行留在原位,已搬走的文件也得搬回去,否则在用条目会丢内容
        for (const auto& n : movedPhys)
        {
            int64_t     sp = n.space;
            std::string back;
            if (m_store->PhysicalPathSync(n.id, sp, back).ok &&
                m_store->MovePath(m_store->TrashEntryPath(n.space, n.id, n.name), back).ok)
                m_store->RemoveTreeSync(m_store->TrashEntryDir(n.space, n.id));
        }
        return ZmFileError(zm_file_err::kInternal, 500, "删除失败");
    }

    ZMJSON out     = ZMJSON::object();
    out["success"] = std::move(success);
    out["failed"]  = std::move(failed);
    out["count"]   = static_cast<int64_t>(out["success"].size());
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::SoftDelete(const std::vector<int64_t>& ids,
                                                  const ZmOpCtx&              ctx)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>([this, ids, ctx]() -> ZMJSON
                                                       { return SoftDeleteSync(ids, ctx); });
}

// ============================================================================
// 回收站列表
// ============================================================================
ZMJSON ZmFileNodeModule::TrashListSync(int64_t space, const ZmListQuery& q, int64_t uid,
                                       bool adminAll)
{
    std::string              where = " WHERE deleted = 1";
    std::vector<std::string> params;
    if (space >= 0)
    {
        where += " AND space = ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(space));
    }
    if (!adminAll)
    {
        // 用户侧:常量条件写死在 SQL 里,不接受任何可绕过的参数
        where += " AND del_owner_uid = ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(uid));
    }
    if (q.mtimeFrom > 0)
    {
        where += " AND delete_time >= ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(q.mtimeFrom));
    }
    if (q.mtimeTo > 0)
    {
        where += " AND delete_time <= ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(q.mtimeTo));
    }
    // 关键词过滤:与空间搜索同口径(口径统一在 ZmAddKeywordCond,别在这里另写一份)
    ZmAddKeywordCond(where, params, q.keyword, {"name"});

    ZMJSON  totalRow = m_db->QueryRowSync("SELECT COUNT(*) AS n FROM nodes" + where, params);
    int64_t total    = zm_file_row_int(totalRow, "n", 0);

    // 排序:回收站没有"修改时间"的语义,认 delete_time;order 参数真正生效
    // (原先只认 sort=name,其余一律 delete_time DESC,升降序写死)
    std::string dir = (q.order == "desc") ? " DESC" : " ASC";
    std::string col = "delete_time";
    if (q.sort == "name")
        col = "name";
    else if (q.sort == "type")
        col = "type";
    else if (q.sort == "size")
        col = "size";
    // id 兜底:同名/同大小/同秒删除时保证顺序稳定,翻页不会漏项或重项
    std::string              order = " ORDER BY " + col + dir + ", id" + dir;
    std::vector<std::string> lp    = params;
    lp.push_back(std::to_string(q.size));
    lp.push_back(std::to_string((q.page - 1) * q.size));
    ZMJSON rows = m_db->QueryRowsSync("SELECT * FROM nodes" + where + order + " LIMIT ?" +
                                          std::to_string(lp.size() - 1) + " OFFSET ?" +
                                          std::to_string(lp.size()),
                                      lp);

    ZMJSON list = ZMJSON::array();
    for (const auto& r : rows)
    {
        ZmFileNode n = ZmFileNode::FromRow(r);
        // 以空间列表同一套字段打底(NodeView),再叠加回收站独有的几项。
        // 别在这里另起一份手写字段表:两边会漂移 —— "类型"列全变"其他"、
        // "删除时间"列显示成"—"就是漏了 ext 与 update_time 造成的
        ZMJSON item    = NodeView(r);
        item["space"]  = n.space;   // 管理端"空间"列按它推导,缺了下发就恒为公共空间
        // 目录的 size 恒为 0(nodes 表语义:目录不占字节),但回收站视图的"大小"列
        // 要给"子树条目数与字节数"(需求 §3.7.2) —— 否则用户看到"1 项 · 0 B",
        // 既估不出彻底删除的耗时,也看不出这块占用有多大
        if (n.type == zm_file::kTypeDir)
        {
            int64_t subItems = 0;
            int64_t subBytes = 0;
            SubtreeStat(m_db, n.id, subItems, subBytes);
            // SubtreeStat 含自身,展示口径与列表视图一致 → 减去自身得到"内容物数量"
            item["items"] = subItems > 0 ? subItems - 1 : 0;
            item["bytes"] = subBytes;
        }
        item["delete_time"]   = n.deleteTime;
        item["del_owner_uid"] = n.delOwnerUid;
        // 原父目录 id:0 = 本来就删在空间根(前端据此区分"空间根"与"原目录已删除")
        item["origin_parent_id"] = n.originParentId;
        // 原路径 = 原父目录的路径;原父目录已删除(或原本就在空间根)时给空串
        std::string path;
        if (n.originParentId != 0 && VisibleSync(n.originParentId))
            path = RelPathSync(n.originParentId);
        item["path"] = path;
        list.push_back(std::move(item));
    }

    // 顶部占用:用户侧 = 我删的;管理侧 = 范围内全量
    int64_t usedSize  = 0;
    int64_t usedItems = 0;
    {
        std::string              uwhere = " WHERE deleted = 1";
        std::vector<std::string> up;
        if (space >= 0)
        {
            uwhere += " AND space = ?" + std::to_string(up.size() + 1);
            up.push_back(std::to_string(space));
        }
        if (!adminAll)
        {
            uwhere += " AND del_owner_uid = ?" + std::to_string(up.size() + 1);
            up.push_back(std::to_string(uid));
        }
        // 目录的 size 为 0:占用必须按子树字节累加,否则"回收站占用"会漏掉被删目录
        // 里的全部文件(用户会看到"删了 1.6 GB 却显示占用 0")
        ZMJSON urows = m_db->QueryRowsSync("SELECT id, type, size FROM nodes" + uwhere, up);
        for (const auto& r : urows)
        {
            const int64_t id   = zm_file_row_int(r, "id", 0);
            const int64_t type = zm_file_row_int(r, "type", 0);
            ++usedItems;
            if (type == zm_file::kTypeDir)
            {
                int64_t si = 0;
                int64_t sb = 0;
                SubtreeStat(m_db, id, si, sb);
                usedSize += sb;
            }
            else
            {
                usedSize += zm_file_row_int(r, "size", 0);
            }
        }
    }

    ZMJSON out         = ZMJSON::object();
    out["total"]       = total;
    out["page"]        = q.page;
    out["size"]        = q.size;
    out["list"]        = std::move(list);
    out["used_size"]   = usedSize;
    out["used_items"]  = usedItems;
    out["retain_days"] = zm_file::kTrashRetainDays;
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::TrashList(int64_t space, const ZmListQuery& q,
                                                 int64_t uid, bool adminAll)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, q, uid, adminAll]() -> ZMJSON
        { return TrashListSync(space, q, uid, adminAll); });
}

// ============================================================================
// 恢复
// ============================================================================
ZMJSON ZmFileNodeModule::RestoreSync(const std::vector<int64_t>& idsIn, const ZmOpCtx& ctx,
                                     bool adminAll)
{
    std::vector<int64_t> ids = DedupeIds(idsIn);
    if (ids.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未指定条目");
    if (ids.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多恢复 2000 个条目");

    ZMJSON success  = ZMJSON::array();
    ZMJSON failed   = ZMJSON::array();
    ZMJSON restored = ZMJSON::array();
    for (int64_t id : ids)
    {
        ZMJSON row      = m_db->NodeRowSync(id);
        auto   notFound = [&]()
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = id;
            f["name"] = row.empty() ? std::to_string(id) : zm_file_row_str(row, "name");
            f["code"] = zm_file_err::kTrashItemNotFound;
            failed.push_back(std::move(f));
        };
        if (row.empty() || zm_file_row_int(row, "deleted", 0) != 1)
        {
            notFound();
            continue;
        }
        ZmFileNode n = ZmFileNode::FromRow(row);
        // 归属校验:不是自己删的与"不存在"同构,不泄露他人回收站内容
        if (!adminAll && n.delOwnerUid != ctx.uid)
        {
            notFound();
            continue;
        }
        // 原位置可用才回原位;否则回空间根
        int64_t parentId = n.originParentId;
        bool    parentOk = false;
        if (parentId == 0)
        {
            parentOk = true;
        }
        else
        {
            ZMJSON prow;
            parentOk = VisibleSync(parentId, prow) &&
                       zm_file_row_int(prow, "type", 0) == zm_file::kTypeDir &&
                       zm_file_row_int(prow, "space", 0) == n.space;
        }
        if (!parentOk)
            parentId = 0;

        m_db->EnsureSpaceSync(n.space);
        m_store->EnsureSpaceRoot(n.space);
        ZmDirLock::Guard guard(*m_lock, n.space, parentId);
        std::string      finalName = FreeNameSync(n.space, parentId, n.name);

        // 物理侧先行:把文件从回收站搬回目标位置。名字被占用时 finalName 已换新名,
        // 所以搬过去即改名,不留"库名变了、磁盘还在旧名"的空壳
        int64_t     backSp = n.space;
        std::string parentPath;
        std::string srcPath = m_store->TrashEntryPath(n.space, n.id, n.name);
        if (!m_store->PhysicalPathSync(parentId, backSp, parentPath).ok ||
            !m_store->Exists(srcPath))
        {
            // 算不出目标目录,或回收站里的内容已丢(存档损坏):恢复出来也是空壳,
            // 如实拒绝,别让用户以为文件回来了
            DEFAULT_LOG_ERROR("ZmFileNodeModule: 恢复内容缺失: {} (id={})", n.name, n.id);
            notFound();
            continue;
        }
        std::string dstPath = parentPath + "\\" + finalName;
        if (!m_store->EnsureDir(parentPath).ok || !m_store->MovePath(srcPath, dstPath).ok)
        {
            DEFAULT_LOG_ERROR("ZmFileNodeModule: 恢复搬移失败: {} (id={})", n.name, n.id);
            notFound();
            continue;
        }

        int64_t          now       = ZmSqliteDb::Now();
        int64_t          newId     = n.id;
        bool             ok        = m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                if (!db.ExecSync("UPDATE nodes SET deleted = 0, delete_time = 0, "
                                                                       "origin_parent_id = 0, del_owner_uid = 0, parent_id = ?1, "
                                                                       "name = ?2, update_time = ?3 WHERE id = ?4 AND deleted = 1",
                                                    {std::to_string(parentId), finalName, std::to_string(now),
                                  std::to_string(n.id)}))
                    return false;
                int64_t changed =
                    zm_file_row_int(db.QueryRowTxSync("SELECT changes() AS n", {}), "n", 0);
                if (changed == 0)
                    return false;
                if (!TouchParentSync(db, parentId, 1))
                    return false;
                std::string path   = RelPathSync(n.id);
                ZMJSON      detail = ZMJSON::object();
                detail["to"]       = path;
                return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                                    zm_file::kActRestore, n.space, n.id,
                                                                    finalName, detail.dump(), ctx.ip, 1);
            });
        if (!ok)
        {
            // 事务失败:刚搬回的文件送回回收站,库行没动,条目留在回收站可重试
            m_store->MovePath(dstPath, srcPath);
            notFound();
            continue;
        }
        // 文件已搬走,回收站里那个条目目录空了:顺手删掉,免得留空壳
        m_store->RemoveTreeSync(m_store->TrashEntryDir(n.space, n.id));
        success.push_back(n.id);
        ZMJSON one  = ZMJSON::object();
        one["id"]   = newId;
        one["name"] = finalName;
        one["path"] = RelPathSync(n.id);
        restored.push_back(std::move(one));
    }

    ZMJSON out      = ZMJSON::object();
    out["success"]  = std::move(success);
    out["failed"]   = std::move(failed);
    out["restored"] = std::move(restored);
    return out;
}

drogon::Task<ZMJSON> ZmFileNodeModule::Restore(const std::vector<int64_t>& ids,
                                               const ZmOpCtx& ctx, bool adminAll)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ids, ctx, adminAll]() -> ZMJSON { return RestoreSync(ids, ctx, adminAll); });
}

// ============================================================================
// 彻底删除(先物理、后删行;回滚单位是顶层条目)
// ============================================================================
ZMJSON ZmFileNodeModule::PurgeSync(const std::vector<int64_t>& idsIn, const ZmOpCtx& ctx,
                                   bool adminAll, ZMJSON* aggOut)
{
    std::vector<int64_t> ids = DedupeIds(idsIn);
    if (ids.empty())
        return ZmFileError(zm_file_err::kBadRequest, 400, "未指定条目");
    if (ids.size() > kMaxBatch)
        return ZmFileError(zm_file_err::kBatchTooLarge, 400, "单次最多删除 2000 个条目");

    ZMJSON  success    = ZMJSON::array();
    ZMJSON  failed     = ZMJSON::array();
    ZMJSON  auditItems = ZMJSON::array();

    // 锁各条目的原父目录(与软删除/新建/改名/移动同一把锁;LockAll 内部按桶号排序防死锁):
    // 否则"正被彻底删除的目录里又被塞进新条目"会留下无对应文件的库行
    std::vector<std::pair<int64_t, int64_t>> purgeLockKeys;
    for (int64_t id : ids)
    {
        ZMJSON row = m_db->NodeRowSync(id);
        if (row.empty() || zm_file_row_int(row, "deleted", 0) != 1)
            continue;
        purgeLockKeys.emplace_back(zm_file_row_int(row, "space", 0),
                                   zm_file_row_int(row, "parent_id", 0));
    }
    auto purgeGuards = m_lock->LockAll(purgeLockKeys);

    int64_t totalBytes = 0;
    for (int64_t id : ids)
    {
        ZMJSON row = m_db->NodeRowSync(id);
        if (row.empty() || zm_file_row_int(row, "deleted", 0) != 1)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = id;
            f["name"] = row.empty() ? std::to_string(id) : zm_file_row_str(row, "name");
            f["code"] = zm_file_err::kTrashItemNotFound;
            failed.push_back(std::move(f));
            continue;
        }
        ZmFileNode n = ZmFileNode::FromRow(row);
        if (!adminAll && n.delOwnerUid != ctx.uid)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = id;
            f["name"] = n.name;
            f["code"] = zm_file_err::kTrashItemNotFound;
            failed.push_back(std::move(f));
            continue;
        }

        int64_t subBytes = 0;
        ZMJSON  subIds   = SubtreeIdsSync(n.id, &subBytes, nullptr);
        int64_t subItems = static_cast<int64_t>(subIds.size());
        // 回收站条目的文件在回收站区(删除时已搬离原位),按条目目录整棵删;
        // 目录本就空/不存在时 RemoveTreeSync 视作成功,不必先探存在
        std::string physPath = m_store->TrashEntryDir(n.space, n.id);
        // ① 先做物理删除:失败则整条保留(行一行都不动),下轮重试
        ZmStoreResult rm = m_store->RemoveTreeSync(physPath);
        if (!rm.ok)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = n.id;
            f["name"] = n.name;
            f["code"] = (rm.code == static_cast<int>(ZmErrCode::Locked) ||
                         rm.code == static_cast<int>(ZmErrCode::AccessDenied))
                            ? zm_file_err::kFileLocked
                            : zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }
        // ② 物理删成功才删行并释放配额
        bool ok = m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                // 指向被删条目的有效分享一并失效(必须在删行之前:失效判定要靠这些行)
                std::vector<int64_t> gone;
                for (const auto& v : subIds)
                {
                    if (v.is_number_integer())
                        gone.push_back(v.get<int64_t>());
                }
                if (!ZmFileDbModule::InvalidateSharesByNodesSync(db, gone))
                    return false;
                if (!db.ExecSync("WITH RECURSIVE sub(id, depth) AS ("
                                 " SELECT id, 0 FROM nodes WHERE id = ?1"
                                 " UNION ALL"
                                 " SELECT n.id, s.depth + 1 FROM nodes n JOIN sub s"
                                 " ON n.parent_id = s.id WHERE s.depth < 64)"
                                 " DELETE FROM nodes WHERE id IN (SELECT id FROM sub);",
                                 {std::to_string(n.id)}))
                    return false;
                return ZmFileDbModule::ApplyUsageSync(db, n.space, -subBytes, -subItems);
            });
        if (!ok)
        {
            ZMJSON f  = ZMJSON::object();
            f["id"]   = n.id;
            f["name"] = n.name;
            f["code"] = zm_file_err::kInternal;
            failed.push_back(std::move(f));
            continue;
        }
        success.push_back(n.id);
        totalBytes += subBytes;
        ZMJSON item  = ZMJSON::object();
        item["id"]   = n.id;
        item["name"] = n.name;
        item["type"] = n.type;
        item["size"] = n.size;
        item["path"] = RelPathSync(n.id);
        auditItems.push_back(std::move(item));
        if (aggOut)
        {
            (*aggOut)["purged"] = zm_file_row_int(*aggOut, "purged", 0) + 1;
            (*aggOut)["bytes"]  = zm_file_row_int(*aggOut, "bytes", 0) + subBytes;
        }
    }

    if (!auditItems.empty())
    {
        m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                return m_audit->RecordBatchSync(db, ctx.uid, ctx.account, zm_file::kActPurge,
                                                -1, auditItems, ctx.ip, 1);
            });
    }

    ZMJSON out     = ZMJSON::object();
    out["success"] = std::move(success);
    out["failed"]  = std::move(failed);
    out["bytes"]   = totalBytes;
    return out;
}

ZMJSON ZmFileNodeModule::PurgeIdsSync(const std::vector<int64_t>& ids, const ZmOpCtx& ctx,
                                      bool adminAll)
{
    return PurgeSync(ids, ctx, adminAll, nullptr);
}

drogon::Task<ZMJSON> ZmFileNodeModule::Purge(const std::vector<int64_t>& ids,
                                             const ZmOpCtx& ctx, bool adminAll)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ids, ctx, adminAll]() -> ZMJSON
        { return PurgeSync(ids, ctx, adminAll, nullptr); });
}

ZMJSON ZmFileNodeModule::PurgeExpiredSync(int64_t beforeTime)
{
    ZMJSON rows = m_db->QueryRowsSync(
        "SELECT id FROM nodes WHERE deleted = 1 AND delete_time > 0 AND delete_time < ?1 "
        "ORDER BY delete_time ASC LIMIT 5000",
        {std::to_string(beforeTime)});
    std::vector<int64_t> ids;
    for (const auto& r : rows)
        ids.push_back(zm_file_row_int(r, "id", 0));
    ZMJSON agg    = ZMJSON::object();
    agg["purged"] = 0;
    agg["bytes"]  = 0;
    if (ids.empty())
        return agg;
    // 保留期清理是系统行为:不受删除者归属限制,审计记 uid=0(系统)
    ZmOpCtx sys;
    sys.uid       = 0;
    sys.account   = "system";
    ZMJSON res    = PurgeSync(ids, sys, true, &agg);
    agg["failed"] = res.contains("failed") ? res["failed"] : ZMJSON::array();
    return agg;
}

drogon::Task<ZMJSON> ZmFileNodeModule::PurgeExpired(int64_t beforeTime)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, beforeTime]() -> ZMJSON { return PurgeExpiredSync(beforeTime); });
}

// ============================================================================
// 清空回收站(任务执行体)
// ============================================================================
ZMJSON ZmFileNodeModule::ClearTrashExec(int64_t space, bool adminAll, const ZmOpCtx& ctx,
                                        ZmTaskHandle* handle)
{
    std::string              where = " WHERE deleted = 1";
    std::vector<std::string> params;
    if (space >= 0)
    {
        where += " AND space = ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(space));
    }
    if (!adminAll)
    {
        where += " AND del_owner_uid = ?" + std::to_string(params.size() + 1);
        params.push_back(std::to_string(ctx.uid));
    }
    ZMJSON rows = m_db->QueryRowsSync("SELECT id, size FROM nodes" + where, params);
    std::vector<int64_t> ids;
    int64_t              totalBytes = 0;
    for (const auto& r : rows)
    {
        ids.push_back(zm_file_row_int(r, "id", 0));
        totalBytes += zm_file_row_int(r, "size", 0);
    }
    ZMJSON agg    = ZMJSON::object();
    agg["purged"] = 0;
    agg["bytes"]  = 0;
    if (ids.empty())
        return agg;

    // 逐条彻底删除:每条独立提交,取消后已完成部分保留
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (handle && handle->Cancelled())
            break;
        ZMJSON one = PurgeSync({ids[i]}, ctx, adminAll, &agg);
        if (handle)
            handle->Progress(zm_file_row_int(agg, "bytes", 0), static_cast<int64_t>(i + 1));
    }
    // 清空动作单独记一条审计(逐条的 purge 记录之外,便于回溯"谁在什么时候清空了回收站")
    if (zm_file_row_int(agg, "purged", 0) > 0)
    {
        ZMJSON detail = ZMJSON::object();
        detail["purged"] = zm_file_row_int(agg, "purged", 0);
        detail["bytes"]  = zm_file_row_int(agg, "bytes", 0);
        detail["space"]  = space;
        m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                 zm_file::kActTrashClear, space, 0, "回收站清空",
                                                 detail.dump(), ctx.ip, 1);
            });
    }
    (void)totalBytes;
    return agg;
}

ZMJSON ZmFileNodeModule::MigrateTrashLayout()
{
    ZMJSON  out   = ZMJSON::object();
    int64_t moved = 0;
    ZMJSON  rows  = m_db->QueryRowsSync(
        "SELECT id, space, parent_id, name FROM nodes WHERE deleted = 1 "
        "ORDER BY delete_time ASC LIMIT 5000",
        {});
    for (const auto& r : rows)
    {
        int64_t     id     = zm_file_row_int(r, "id", 0);
        int64_t     space  = zm_file_row_int(r, "space", 0);
        int64_t     parent = zm_file_row_int(r, "parent_id", 0);
        std::string name   = zm_file_row_str(r, "name");

        std::string dst = m_store->TrashEntryPath(space, id, name);
        if (m_store->Exists(dst))
            continue; // 回收站区已有内容 = 已迁过
        // 原位置被在用条目占用:那份文件属于新文件,不是这条回收站条目的
        ZMJSON dup = m_db->QueryRowSync(
            "SELECT id FROM nodes WHERE space = ?1 AND parent_id = ?2 AND deleted = 0 "
            "AND name = ?3 COLLATE NOCASE LIMIT 1",
            {std::to_string(space), std::to_string(parent), name});
        if (!dup.empty())
            continue;
        int64_t     sp = space;
        std::string src;
        if (!m_store->PhysicalPathSync(id, sp, src).ok || !m_store->Exists(src))
            continue;
        if (!m_store->EnsureDir(m_store->TrashEntryDir(space, id)).ok)
            continue;
        if (!m_store->MovePath(src, dst).ok)
        {
            DEFAULT_LOG_WARN("ZmFileNodeModule: 回收站条目文件搬迁失败,跳过: {}", name);
            continue;
        }
        ++moved;
    }
    out["moved"] = moved;
    if (moved > 0)
        DEFAULT_LOG_INFO("ZmFileNodeModule: 回收站条目文件补搬完成,共 {} 条", moved);
    return out;
}

// ============================================================================
// 文件条目落库(上传入位 / 秒传;在调用方事务内执行)
// ============================================================================
bool ZmFileNodeModule::InsertFileSync(ZmSqliteDb& db, int64_t space, int64_t parentId,
                                      const std::string& name, int64_t size,
                                      const std::string& ext, const std::string& hash,
                                      int64_t ownerUid, int64_t& newId)
{
    int64_t now = ZmSqliteDb::Now();
    if (!db.ExecSync(
            "INSERT INTO nodes(space,parent_id,type,name,size,ext,hash,owner_uid,items,"
            "create_time,update_time,deleted,delete_time,origin_parent_id,del_owner_uid) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,0,?9,?9,0,0,0,0)",
            {std::to_string(space), std::to_string(parentId),
             std::to_string(zm_file::kTypeFile), name, std::to_string(size), ext, hash,
             std::to_string(ownerUid), std::to_string(now)}))
        return false;
    newId =
        zm_file_row_int(db.QueryRowTxSync("SELECT last_insert_rowid() AS id", {}), "id", 0);
    if (!ZmFileDbModule::ApplyUsageSync(db, space, size, 1))
        return false;
    return TouchParentSync(db, parentId, 1);
}
