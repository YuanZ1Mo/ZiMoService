#ifndef ZM_MODULE_FILE_DB_H
#define ZM_MODULE_FILE_DB_H

// ============================================================================
// ZmFileDbModule:文件中心数据访问模块
// 职责:filehub.db 全新建表(9 表 + 索引,含部分唯一索引)与公共空间种子、
// PATH 组装所需的父链查询、空间用量/配额、通用计数表(write_limits)、
// 每日 03:00 周期清理。
// 连接与事务模型继承自公共库 ZmSqliteDb,与用户系统 ZmDbModule 同型。
// 槽位:本类是 filehub.db 全部 SQL 的唯一出处,业务模块不自行拼 SQL。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>
#include <zm_util_sqlite.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ZmFileDbModule : public ZmSqliteDb
{
  public:
    /**
 * @brief 物理侧清理动作(由装配方注入)
 *
 * 定时清理中的回收站保留期清理、分片目录回收、打包缓存清理、每日一致性校验
 * 都要动物理文件或跨表业务逻辑,由上层(ServicePortal)注入实现,避免底层模块
 * 反向依赖 Store / Node / Upload。
 */
    struct CleanupHooks
    {
        std::function<void(int64_t now)> purgeExpiredTrash; ///< 回收站保留期到期清理
        std::function<void(int64_t now)> purgeUploadChunks; ///< 过期上传会话与分片目录
        std::function<void(int64_t now)> cleanCache;        ///< 打包缓存清理
        std::function<void(int64_t now)> verifyConsistency; ///< 全空间一致性校验
    };

    ZmFileDbModule();
    ~ZmFileDbModule() override;

    /**
 * @brief 打开库 + 建表建索引 + 写入公共空间行并创建其物理目录
 *
 * @param dbPath 库文件绝对路径(exe 同级 db\filehub\filehub.db)
 * @param rootDir 文件中心根目录(exe 同级 modules\filehub)
 * @return true 成功
 */
    bool Init(const std::string& dbPath, const std::string& rootDir);

    /// @brief 注册事件循环启动回调:每日 03:00 执行 CleanupOnce
    void StartPeriodicCleanup();

    /// @brief 注入物理侧清理钩子(装配阶段调用一次;未注入时对应项跳过)
    void SetCleanupHooks(const CleanupHooks& hooks) { m_hooks = hooks; }

    /// @return 文件中心根目录
    const std::string& RootDir() const { return m_rootDir; }

    // ── 路径装配(供 ZmFileStoreModule) ──
    /**
 * @brief 取条目的所属空间与相对空间根的路径
 *
 * 沿 parent_id 链取 name 逐级拼接(以 `\` 分隔),**不过滤 deleted** ——
 * 回收站条目的物理文件仍在原位,彻底删除时必须能算出路径。
 *
 * @param nodeId 条目 id(0 = 空间根,此时返回空相对路径)
 * @param space [out] 条目所属空间
 * @param relPath [out] 相对空间根的路径(nodeId=0 时为空串)
 * @return true 条目存在(或 nodeId=0);false 条目不存在
 */
    bool PathPartsSync(int64_t nodeId, int64_t& space, std::string& relPath);

    /**
 * @brief 按 id 取条目行(不过滤 deleted)
 *
 * @param nodeId 条目 id
 * @return 行(不存在返回空对象)
 */
    ZMJSON NodeRowSync(int64_t nodeId);

    /**
 * @brief 取条目行的所属空间(仅空间列,供权限判定前置用)
 *
 * @param nodeId 条目 id
 * @return 空间;条目不存在返回 -1
 */
    int64_t NodeSpaceSync(int64_t nodeId);

    // ── 空间 ──
    /// @return 全部空间行(按 space 升序)
    drogon::Task<ZMJSON> ListSpaces();
    /// @brief 取单个空间行
    /// @param space 空间号
    /// @return 行(不存在返回空对象)
    drogon::Task<ZMJSON> GetSpace(int64_t space);

    /// @brief 取空间行(同步版;供工作池线程内的配额判定使用)
    /// @param space 空间号
    /// @return 行(不存在返回空对象)
    ZMJSON SpaceRowSync(int64_t space);
    /**
 * @brief 懒创建个人空间(幂等:库中补行 + 物理目录)
 *
 * @param space 空间号(>0)
 * @return true 已就绪
 */
    drogon::Task<bool> EnsureSpace(int64_t space);
    /// @brief 懒创建个人空间(同步版;幂等:库中补行 + 物理目录)
    ///
    /// 供写入路径(上传/新建/移动/复制)在工作池线程内调用:个人空间的行与目录
    /// 是"首次用到时创建",缺行会让配额校验失去依据(SpaceRowSync 查不到行)。
    ///
    /// @param space 空间号(0 = 公共空间,建库时已存在)
    /// @return true 已就绪
    bool EnsureSpaceSync(int64_t space);

    /// @brief 设置配额(0 = 不限)
    drogon::Task<bool> SetQuota(int64_t space, int64_t quota);

    // ── 用量记账(事务内调用;delta 为增量,可为负) ──
    /**
 * @brief 空间用量增减
 *
 * @param db 事务连接(来自 WithTx 回调形参)
 * @param space 空间号
 * @param dSize 字节增量(可为负)
 * @param dItems 条目增量(可为负)
 * @return true 写入成功
 */
    static bool ApplyUsageSync(ZmSqliteDb& db, int64_t space, int64_t dSize, int64_t dItems);

    /**
 * @brief 重算空间用量(一致性校验结束后调用)
 *
 * 计入 deleted=1 的占用(回收站占配额)。
 *
 * @param db 事务连接
 * @param space 空间号
 * @return true 写入成功
 */
    static bool RecountUsageSync(ZmSqliteDb& db, int64_t space);

    /**
 * @brief 统计"我删除的"回收站占用(用户侧展示口径,不影响配额口径)
 *
 * @param db 连接(事务内传事务连接,否则传读连接)
 * @param space 空间号
 * @param uid 删除者
 * @param bytes [out] 字节数
 * @param items [out] 条目数
 */
    static void TrashUsageSync(ZmSqliteDb& db, int64_t space, int64_t uid, int64_t& bytes,
                               int64_t& items, bool tx = false);

    // ── 通用计数表 write_limits(本期用于提取码失败冷却) ──
    /**
 * @brief 取窗口内计数
 *
 * @param db 连接
 * @param scope 计数场景(如 share_pwd_fail)
 * @param dimKey 计数维度键
 * @param tx true = 走写连接(事务内),false = 走读连接
 * @return 计数;无记录返回 0
 */
    static int64_t LimitCountSync(ZmSqliteDb& db, const std::string& scope,
                                  const std::string& dimKey, bool tx = false);

    /**
 * @brief 计数 +1(窗口过期则重置为 1)
 *
 * @param db 事务连接
 * @param scope 计数场景
 * @param dimKey 计数维度键
 * @param windowSec 窗口长度(秒)
 * @param now 当前时间
 * @return true 写入成功
 */
    static bool BumpLimitSync(ZmSqliteDb& db, const std::string& scope,
                              const std::string& dimKey, int64_t windowSec, int64_t now);

    /// @brief 清除计数(校验成功后重置连错计数)
    static bool ClearLimitSync(ZmSqliteDb& db, const std::string& scope,
                               const std::string& dimKey);

    /// @brief 取窗口起点(供冷却判定:"窗口未过期且计数达上限" → 冷却中)
    static int64_t LimitWindowStartSync(ZmSqliteDb& db, const std::string& scope,
                                        const std::string& dimKey);

    /// @brief 立即执行一轮周期清理(管理端"立即执行保留期清理"亦复用之)
    void CleanupOnce();

    // ── 建表语句(公开给一致性校验之外的维护脚本用) ──
    /// @return 库文件物理大小(字节);不可用返回 0
    drogon::Task<int64_t> DbFileSize();

  private:
    bool BuildSchema();
    bool SeedSpaceRoot();
    /// @brief 创建空间物理根目录(space\0 与空间缓存目录)
    bool EnsureSpaceDirs(int64_t space) const;

    std::string  m_rootDir;
    CleanupHooks m_hooks;
};

#endif // ZM_MODULE_FILE_DB_H
