#ifndef ZM_DIR_LOCK_H
#define ZM_DIR_LOCK_H

// ============================================================================
// ZmDirLock:目录级写锁
// 键 = (space, dir_id);同键串行、异键并发;实现为按桶散列的互斥量数组,
// 不引入全局大锁。
// 一次锁多个目录时**按桶号排序去重后加锁**(不是按 key 排序 —— 两个不同目录
// 可能落在同一个桶上,按 key 排会对同一把 std::mutex 加两次锁而死锁);
// 全局加锁顺序一致,不会出现环路等待。
// 持锁范围只覆盖"冲突裁决 + DB 写入"窗口,长耗时物理 IO 在锁外做。
// ============================================================================

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

class ZmDirLock
{
  public:
    /// 桶数量(取 2 的幂,散列与去重成本低)
    static constexpr size_t kBuckets = 256;

    /**
 * @brief 目录写锁的 RAII 句柄(析构自动解锁)
 *
 * 同一桶被多个键命中时(桶数远小于目录数,必然发生),只有第一个键真正加锁,
 * 其余键共享它 —— 否则会对同一把非递归互斥量重复加锁而死锁。
 */
    class Guard
    {
      public:
        /**
 * @brief 单键加锁(立即锁定该目录所在桶)
 *
 * @param lock 所属锁对象
 * @param space 空间号
 * @param dirId 目录 id(0 = 空间根)
 */
        Guard(ZmDirLock& lock, int64_t space, int64_t dirId);
        ~Guard();

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;

        /**
 * @brief 内部构造:登记"桶 + 是否由本句柄真正持锁",**不再执行加锁**
 *
 * 供 ZmDirLock::LockAll 在按桶号升序完成加锁后构造句柄用。
 *
 * @param lock 所属锁对象
 * @param idx 桶下标
 * @param owned true 时析构本句柄会解锁该桶
 */
        Guard(ZmDirLock* lock, size_t idx, bool owned);

      private:
        ZmDirLock* m_lock  = nullptr;
        size_t     m_idx   = 0;
        bool       m_owned = false; ///< true 时析构需解锁
    };

    /**
 * @brief 一次锁多目录(内部按桶号升序去重后加锁)
 *
 * @param keys (space, dir_id) 列表(可含重复)
 * @return 句柄集合,与传入 keys 一一对应
 */
    std::vector<Guard> LockAll(const std::vector<std::pair<int64_t, int64_t>>& keys);

    /// @brief (space, dir_id) → 桶下标(**必须先定桶、再排序**)
    static size_t BucketOf(int64_t space, int64_t dirId);

  private:
    /// @brief 取的桶互斥量
    std::mutex& Bucket(size_t idx) { return m_buckets[idx]; }

    std::array<std::mutex, kBuckets> m_buckets;
};

#endif // ZM_DIR_LOCK_H
