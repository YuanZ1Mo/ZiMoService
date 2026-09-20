#ifndef ZM_MODULE_FILE_NODE_H
#define ZM_MODULE_FILE_NODE_H

// ============================================================================
// ZmFileNodeModule:条目逻辑模块
// 职责:可见树查询(列表/面包屑/搜索/详情)、结构变更(新建/改名/移动/复制/
// 软删除/恢复/彻底删除)、配额记账、目录 items 与父目录时间戳随动。
// 路径不来自客户端:所有入参是 id,物理路径一律由库里 name 重新拼出。
// 子树操作统一用 WITH RECURSIVE 展开,不递归查库。
// 可见树约定:自身 deleted=0 且祖先链上无 deleted=1;命中不可见条目返回 404。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>
#include <zm_util_sqlite.h>

#include <cstdint>
#include <string>
#include <vector>

class ZmFileDbModule;
class ZmFileStoreModule;
class ZmDirLock;
class ZmFileAuditModule;
class ZmTaskHandle;

/// 操作者上下文(审计与权限判定共用)
struct ZmOpCtx
{
    int64_t     uid = 0;
    std::string account;
    std::string ip;
};

/// 列表查询参数(列表/搜索/回收站共用)
struct ZmListQuery
{
    std::string sort  = "name"; ///< name | size | mtime | type(回收站列表另有 delete_time)
    std::string order = "asc";  ///< asc | desc
    int         page  = 1;
    int         size  = 200;
    std::string typeFilter;    ///< dir|doc|img|vid|aud|zip|oth(空 = 不限)
    std::string keyword;       ///< 名称关键词过滤(空 = 不限;回收站列表按它过滤)
    int64_t     mtimeFrom = 0; ///< 修改时间起点(0 = 不限;回收站列表按删除时间判)
    int64_t     mtimeTo   = 0; ///< 修改时间终点(0 = 不限;回收站列表按删除时间判)
};

/// 条目行(库中 nodes 行的强类型视图)
struct ZmFileNode
{
    int64_t     id       = 0;
    int64_t     space    = 0;
    int64_t     parentId = 0;
    int         type     = 0; ///< 1=目录 2=文件
    std::string name;
    int64_t     size = 0;
    std::string ext;
    std::string hash;
    int64_t     ownerUid       = 0;
    int64_t     items          = 0;
    int64_t     createTime     = 0;
    int64_t     updateTime     = 0;
    int         deleted        = 0;
    int64_t     deleteTime     = 0;
    int64_t     originParentId = 0;
    int64_t     delOwnerUid    = 0;

    /// @brief 由查询结果行填充
    /// @param row nodes 表的一行
    /// @return 填充后的条目
    static ZmFileNode FromRow(const ZMJSON& row);
};

class ZmFileNodeModule
{
  public:
    ZmFileNodeModule(ZmFileDbModule* db, ZmFileStoreModule* store, ZmDirLock* lock,
                     ZmFileAuditModule* audit);
    ~ZmFileNodeModule();

    // ── 空间权限(个人空间仅本人) ──
    /// @return 该空间对 uid 是否可读写(公共空间全部登录用户可写)
    static bool SpaceWritable(int64_t space, int64_t uid);

    // ── 名称与路径校验(上传/分享等模块共用) ──
    /// @brief 名称合法性(长度/非法字符/保留名/首尾空格与点)
    /// @param name 待校验名称
    /// @param message [out] 失败原因
    /// @return true 合法
    static bool ValidateName(const std::string& name, std::string& message);
    /// @brief 生成自动重命名候选:`name (1).ext`、`name (2).ext`…
    static std::string DupName(const std::string& name, int index);
    /// @brief 由名称取小写扩展名(无扩展名返回空)
    static std::string ExtOf(const std::string& name);

    // ── 浏览 ──
    /// @brief 列目录(文件夹恒在前)
    /// @param space 空间;dirId 目录 id(0 = 空间根)
    /// @param q 排序/分页/筛选
    /// @return {total,page,size,list,breadcrumb,space};目录不存在 → {error:{...}}
    drogon::Task<ZMJSON> List(int64_t space, int64_t dirId, const ZmListQuery& q);
    /// @brief 递归搜索当前目录及子目录(上限 500 条)
    /// @param keyword 关键词(1~64 字符;空 = 直接返回空结果)
    /// @return {total,page,size,truncated,list};list 条目附 path
    drogon::Task<ZMJSON> Search(int64_t space, int64_t dirId, const std::string& keyword,
                                const ZmListQuery& q);
    /**
 * @brief 在若干基准的子树内递归搜索,给出每条"所在目录"
 *
 * 与 Search 的三点差别,都来自"一次搜索要跨多个并列的根":
 *   · 基准可以给多个(多选分享的顶层是若干并列条目,结果要跨根合并);
 *   · 返回的 path 是**所在目录**(相对所属基准,不含条目自身名称),空串表示就在基准之下 ——
 *     分享页要按它显示"位置",不能带出分享者上层目录的名字,也不能重复条目自己的名称;
 *   · 排序与分页由本方法一次做完;跨基准时 depth 各按自己的基准算,仍是由近及远。
 *
 * @param space          空间
 * @param anchorIds      搜索基准(1~N 个条目;文件没有子项,只会作为自身参与匹配)
 * @param keyword        关键词(1~64 字符;空 = 空结果)
 * @param excludeAnchors true = 基准自身不作为结果(基准是访客正停留的那个目录时用)
 * @param q              排序/分页/类型筛选
 * @return {total,page,size,truncated,list};list 条目附 path(所在目录)、depth、root_id(命中所属基准)
 */
    drogon::Task<ZMJSON> SearchInTree(int64_t space, const std::vector<int64_t>& anchorIds,
                                      const std::string& keyword, bool excludeAnchors,
                                      const ZmListQuery& q);
    /// @brief 单条目详情(附相对空间根的路径)
    /// @param nodeId 条目 id
    /// @param viewerUid 查看者 uid;条目所属空间对其不可写时按"不存在"处理(不泄露名称与路径)
    /// @param adminAll true = 不校验归属(管理端,或已另行校验过归属的调用方)
    /// @return 条目对象;不可见或无权访问 → {error:{...}}
    drogon::Task<ZMJSON> Detail(int64_t nodeId, int64_t viewerUid, bool adminAll);
    /// @brief 统计条目子树规模(任务执行体;由编排层建任务后在工作池内调用)
    /// @param ids 条目集合
    /// @param viewerUid 操作者 uid;非本人空间的条目计入 skipped(规模不外泄)
    /// @param adminAll true = 不校验归属
    /// @param handle 任务句柄(上报进度/响应取消;可为空 = 无任务)
    /// @return {items,bytes,skipped}
    ZMJSON StatExec(const std::vector<int64_t>& ids, int64_t viewerUid, bool adminAll,
                    ZmTaskHandle* handle);

    // ── 结构变更 ──
    /// @brief 新建目录
    /// @return {id,name,create_time};失败 → {error:{code,status,message}}
    drogon::Task<ZMJSON> Mkdir(int64_t space, int64_t parentId, const std::string& name,
                               const ZmOpCtx& ctx);
    /// @brief 按相对路径逐级建目录(幂等;唯一接受客户端路径字符串的入口)
    /// @param paths 相对路径数组(每条以 / 分段,逐段校验)
    /// @param batch true = 批量接口(返回 {created,reused,roots}),false = 单条(返回 {id,created})
    drogon::Task<ZMJSON> EnsureDirs(int64_t space, int64_t parentId,
                                    const std::vector<std::string>& paths, bool batch,
                                    const ZmOpCtx& ctx);
    /// @brief 重命名(单条)
    drogon::Task<ZMJSON> Rename(int64_t nodeId, const std::string& name, const ZmOpCtx& ctx);
    /// @brief 批量移动(可跨空间;冲突裁决整批、执行逐条)
    drogon::Task<ZMJSON> Move(const std::vector<int64_t>& ids, int64_t targetSpace,
                              int64_t targetDir, const std::string& conflict,
                              const ZmOpCtx& ctx);
    /// @brief 复制(可跨空间);超阈值由编排层转异步任务(见 CopyExec)
    /// @return {copied:[],skipped:[],failed:[]}
    drogon::Task<ZMJSON> Copy(const std::vector<int64_t>& ids, int64_t targetSpace,
                              int64_t targetDir, const std::string& conflict,
                              const ZmOpCtx& ctx);
    /// @brief 复制任务执行体(异步路径)
    /// @param handle 任务句柄(上报进度/响应取消;可为空 = 无任务)
    /// @return {copied:[],skipped:[],failed:[],task_no?}
    ZMJSON CopyExec(const std::vector<int64_t>& ids, int64_t targetSpace, int64_t targetDir,
                    const std::string& conflict, const ZmOpCtx& ctx, ZmTaskHandle* handle);
    /// @brief 复制规模预估(供调用方判断同步/异步)
    /// @return {items, bytes}
    drogon::Task<ZMJSON> EstimateCopy(const std::vector<int64_t>& ids);

    // ── 供其他模块复用的通用操作(上传入位、回收站、分享子树校验) ──
    /// @brief 同名校验(不区分大小写,排除自身)
    /// @param dupId [out] 冲突条目 id(命中时有效)
    /// @param dupType [out] 冲突条目类型(1=目录 2=文件)
    /// @return true 存在同名条目
    bool ConflictSync(int64_t space, int64_t parentId, const std::string& name,
                      int64_t excludeId, int64_t& dupId, int& dupType);

    /// @brief 在目录锁内生成不冲突的目标名(rename 策略:`name (1).ext` 递增)
    /// @param space 空间;parentId 目标目录;name 原始名
    /// @return 首个不冲突的名称
    std::string FreeNameSync(int64_t space, int64_t parentId, const std::string& name);

    /// @brief 文件条目落库(上传入位/秒传用;在调用方事务内执行)
    ///
    /// 同时维护空间用量与父目录 items/时间戳。
    ///
    /// @param db 调用方事务连接
    /// @param space 空间;parentId 目标目录;name 最终文件名
    /// @param size 字节数;ext 小写扩展名(无则空);hash 整文件 SHA-256(可空)
    /// @param ownerUid 创建者
    /// @param newId [out] 新条目 id
    /// @return true 落库成功
    static bool InsertFileSync(ZmSqliteDb& db, int64_t space, int64_t parentId,
                               const std::string& name, int64_t size, const std::string& ext,
                               const std::string& hash, int64_t ownerUid, int64_t& newId);

    /// @brief 父目录 items/update_time 随动(parentId=0 时无操作)
    static bool TouchParentSync(ZmSqliteDb& db, int64_t parentId, int64_t dItems);

    /// @brief 条目行 → 对外 JSON(id/type/name/size/ext/items/owner_uid/时间)
    /// (分享虚拟根等跨模块视图复用;owner_name 由编排层补齐)
    static ZMJSON NodeView(const ZMJSON& row);
    /// @brief 列表里的目录行补子树字节(字段 bytes);跨模块构列表时复用
    static void FillDirBytes(ZmFileDbModule* db, ZMJSON& list);
    /**
 * @brief 列表排序子句(目录恒在前,末位以名称与 id 兜底保证分页稳定)
 *
 * 跨模块构列表时复用:分享页顶层的"虚拟根"是若干并列条目、不在同一条 SQL 里,
 * 排序得另写查询,口径必须与本模块的列目录完全一致。
 *
 * @param q 排序键 name|size|mtime|type 与方向 asc|desc(其余取值按 name 处理)
 * @return SQL 的 ORDER BY 片段
 */
    static std::string OrderByClause(const ZmListQuery& q);

    /// @brief 软删除(批量;只标记顶层条目)
    /// @return {success,failed,count}
    drogon::Task<ZMJSON> SoftDelete(const std::vector<int64_t>& ids, const ZmOpCtx& ctx);

    // ── 回收站 ──
    /// @brief 回收站列表(用户侧只含自己删的;adminAll=false 时强制 del_owner_uid 条件)
    /// @param space 空间(adminAll=true 时 -1 表示全部空间)
    /// @return {total,page,size,list,used_size,used_items,retain_days}
    drogon::Task<ZMJSON> TrashList(int64_t space, const ZmListQuery& q, int64_t uid,
                                   bool adminAll);
    /// @brief 恢复(批量);原位置不可用则回到空间根并自动重命名
    /// @return {success,failed,restored:[{id,name,path}]}
    drogon::Task<ZMJSON> Restore(const std::vector<int64_t>& ids, const ZmOpCtx& ctx,
                                 bool adminAll);
    /// @brief 彻底删除(批量;先物理后删行,回滚单位是顶层条目)
    /// @return {success,failed}
    drogon::Task<ZMJSON> Purge(const std::vector<int64_t>& ids, const ZmOpCtx& ctx,
                               bool adminAll);
    /// @brief 彻底删除(同步版;管理端在工作池内直接调用)
    /// @param ids 顶层条目;ctx 操作者;adminAll true = 不受归属限制
    /// @return {success, failed, bytes}
    ZMJSON PurgeIdsSync(const std::vector<int64_t>& ids, const ZmOpCtx& ctx, bool adminAll);

    /// @brief 保留期到期清理(周期任务;不受归属限制,返回清理统计)
    /// @param beforeTime 删除时间早于该时刻的条目
    /// @return {purged, bytes}
    drogon::Task<ZMJSON> PurgeExpired(int64_t beforeTime);
    /// @brief 同上,同步版(供周期清理的工作池线程内调用)
    /// @param beforeTime 删除时间早于该时刻的条目
    /// @return {purged, bytes, failed}
    ZMJSON PurgeExpiredSync(int64_t beforeTime);
    /// @brief 清空回收站(任务执行体,type=6)
    /// @param space 空间;adminAll=true 时清全部空间
    /// @param handle 任务句柄(上报进度/响应取消;可为空 = 无任务)
    /// @return {purged,bytes}
    ZMJSON ClearTrashExec(int64_t space, bool adminAll, const ZmOpCtx& ctx,
                          ZmTaskHandle* handle);

    /**
 * @brief 把历史回收站条目的文件补搬进回收站区(启动期调用;幂等)
 *
 * 软删除曾只标记数据库、不搬文件,这些条目的文件还留在原位置;而恢复与彻底删除
 * 都以回收站区为准,不补搬就恢复不出内容。只搬"回收站区没有、原位置还在、
 * 且原位置没有被在用条目占用"的条目 —— 末一条是防呆:同名新文件已落在原位置时,
 * 那份文件不属于回收站条目,搬了就是抢新文件。搬完条件即不再成立,重复调用无副作用。
 *
 * @return {moved} 本次搬移的条目数(0 = 无需迁移)
 */
    ZMJSON MigrateTrashLayout();

    // ── 供其他模块复用的判定 ──
    /**
 * @brief 可见树判定:条目存在且 deleted=0 且祖先链上无 deleted=1
 *
 * @param nodeId 条目 id
 * @param row [out] 命中时输出节点行
 * @return true 可见
 */
    bool VisibleSync(int64_t nodeId, ZMJSON& row);
    /// @brief 同上,但不输出行
    bool VisibleSync(int64_t nodeId);
    /**
 * @brief 取节点相对空间根的路径(用于展示与审计)
 *
 * @param nodeId 条目 id(0 = 空间根)
 * @param space [out] 所属空间
 * @return 相对路径(以 / 分隔);条目不存在返回空串
 */
    std::string RelPathSync(int64_t nodeId, int64_t* space = nullptr);
    /// @brief 子树展开(含自身;返回 id 列表与规模)
    /// @param bytes [out] 子树文件字节总数;items [out] 子孙条目数
    ZMJSON SubtreeIdsSync(int64_t nodeId, int64_t* bytes = nullptr, int64_t* items = nullptr);
    /**
 * @brief 一批条目子树内最新的 update_time(内容指纹)
 *
 * 用于判断"缓存下来的东西是否还代表当前内容":子树里任何一条被增删改都会顶起
 * 相关行的 update_time(父目录由 TouchParentSync 随动),取整棵子树的最大值即可
 * 发现深层改动。
 *
 * @param ids 条目 id 列表
 * @return 子树内 MAX(update_time);ids 为空或都不可见返回 0
 */
    int64_t SubtreeMaxUpdateSync(const std::vector<int64_t>& ids);
    /// @brief 目标空间配额预检(个人空间)
    /// @param addBytes 本次将新增的字节数
    /// @return true 配额足够(公共空间或不限量恒 true)
    bool QuotaOkSync(int64_t space, int64_t addBytes);

  private:
    // ── 内部同步实现(工作池线程内调用) ──
    ZMJSON ListSync(int64_t space, int64_t dirId, const ZmListQuery& q);
    ZMJSON SearchSync(int64_t space, int64_t dirId, const std::string& keyword,
                      const ZmListQuery& q);
    ZMJSON SearchInTreeSync(int64_t space, const std::vector<int64_t>& anchorIds,
                            const std::string& keyword, bool excludeAnchors, const ZmListQuery& q);
    ZMJSON MkdirSync(int64_t space, int64_t parentId, const std::string& name,
                     const ZmOpCtx& ctx);
    ZMJSON RenameSync(int64_t nodeId, const std::string& name, const ZmOpCtx& ctx);
    ZMJSON MoveSync(const std::vector<int64_t>& ids, int64_t targetSpace, int64_t targetDir,
                    const std::string& conflict, const ZmOpCtx& ctx);
    ZMJSON CopySync(const std::vector<int64_t>& ids, int64_t targetSpace, int64_t targetDir,
                    const std::string& conflict, const ZmOpCtx& ctx, ZmTaskHandle* handle);
    ZMJSON SoftDeleteSync(const std::vector<int64_t>& ids, const ZmOpCtx& ctx);
    ZMJSON TrashListSync(int64_t space, const ZmListQuery& q, int64_t uid, bool adminAll);
    ZMJSON RestoreSync(const std::vector<int64_t>& ids, const ZmOpCtx& ctx, bool adminAll);
    ZMJSON PurgeSync(const std::vector<int64_t>& ids, const ZmOpCtx& ctx, bool adminAll,
                     ZMJSON* aggOut);

    /// @brief 祖先链上是否存在 deleted=1
    bool HiddenByAncestorSync(int64_t nodeId);
    /// @brief 深度校验:条目所在层级 + 追加段数 ≤ 32
    bool DepthOkSync(int64_t parentId, int64_t addLevels, std::string& message);
    /// @brief 目录树自顶向下构建(ensure/ensure_batch 共用)
    ZMJSON EnsureDirsSync(int64_t space, int64_t parentId,
                          const std::vector<std::string>& paths, bool batch,
                          const ZmOpCtx& ctx);
    /// @brief 类型筛选的扩展名清单集合(SQL IN 片段,调用方负责绑定参数)
    static std::string TypeFilterClause(const ZmListQuery&        q,
                                        std::vector<std::string>& params);

    ZmFileDbModule*    m_db    = nullptr;
    ZmFileStoreModule* m_store = nullptr;
    ZmDirLock*         m_lock  = nullptr;
    ZmFileAuditModule* m_audit = nullptr;
};

#endif // ZM_MODULE_FILE_NODE_H
