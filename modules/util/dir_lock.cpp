#include "dir_lock.h"

#include <algorithm>

// ============================================================================
// 桶散列
// ============================================================================
size_t ZmDirLock::BucketOf(int64_t space, int64_t dirId)
{
    // 空间与目录 id 各取一半位做混合,避免"同空间目录 id 连续"退化成线性探测
    uint64_t s = static_cast<uint64_t>(space) * 0x9E3779B97F4A7C15ULL;
    uint64_t d = static_cast<uint64_t>(dirId) * 0xC2B2AE3D27D4EB4FULL;
    uint64_t h = s ^ (d + 0x165667B19E3779F9ULL + (s << 6) + (s >> 2));
    return static_cast<size_t>(h & (kBuckets - 1));
}

// ============================================================================
// Guard
// ============================================================================
ZmDirLock::Guard::Guard(ZmDirLock& lock, int64_t space, int64_t dirId)
    : m_lock(&lock), m_idx(BucketOf(space, dirId)), m_owned(true)
{
    m_lock->Bucket(m_idx).lock();
}

ZmDirLock::Guard::Guard(ZmDirLock* lock, size_t idx, bool owned)
    : m_lock(lock), m_idx(idx), m_owned(owned)
{
    // 不加锁:调用方(多键加锁)已按桶号升序完成加锁,这里只登记持有关系
}

ZmDirLock::Guard::~Guard()
{
    if (m_owned && m_lock)
        m_lock->Bucket(m_idx).unlock();
}

ZmDirLock::Guard::Guard(Guard&& other) noexcept
    : m_lock(other.m_lock), m_idx(other.m_idx), m_owned(other.m_owned)
{
    other.m_lock  = nullptr;
    other.m_owned = false;
}

ZmDirLock::Guard& ZmDirLock::Guard::operator=(Guard&& other) noexcept
{
    if (this == &other)
        return *this;
    if (m_owned && m_lock)
        m_lock->Bucket(m_idx).unlock();
    m_lock        = other.m_lock;
    m_idx         = other.m_idx;
    m_owned       = other.m_owned;
    other.m_lock  = nullptr;
    other.m_owned = false;
    return *this;
}

// ============================================================================
// 多键加锁
// ============================================================================
std::vector<ZmDirLock::Guard>
ZmDirLock::LockAll(const std::vector<std::pair<int64_t, int64_t>>& keys)
{
    // 先定桶,再按桶号升序排序去重(不能按 key 排序:不同 key 可能同桶)
    std::vector<size_t> buckets;
    buckets.reserve(keys.size());
    for (const auto& k : keys)
        buckets.push_back(BucketOf(k.first, k.second));

    std::vector<size_t> order(buckets);
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    // 按桶号升序加锁:全局顺序一致 → 不会环路等待
    for (size_t idx : order)
        Bucket(idx).lock();

    // 按 keys 原顺序构造句柄;同一桶只有首个键持锁(其余共享,不再加锁)
    std::vector<Guard> result;
    result.reserve(keys.size());
    std::vector<size_t> owned;
    owned.reserve(order.size());
    for (size_t b : buckets)
    {
        bool first = std::find(owned.begin(), owned.end(), b) == owned.end();
        if (first)
            owned.push_back(b);
        result.emplace_back(this, b, first);
    }
    return result;
}
