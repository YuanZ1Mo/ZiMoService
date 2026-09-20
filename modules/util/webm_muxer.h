#ifndef ZM_UTIL_WEBM_MUXER_H
#define ZM_UTIL_WEBM_MUXER_H

#include <cstdint>
#include <utility>
#include <vector>

/**
 * @brief WebM(EBML)直播封装器:产出初始化段与"每片一簇"
 *
 * 供服务器音频模块把 Opus 帧封装成浏览器 MediaSource 可直接追加的字节流。
 * 只有两种产出物:
 *   - 初始化段(EBML 头 + Segment(未知长度) + Info + Tracks):一次生成,长期复用;
 *   - 分片(一个 Cluster):满 kFramesPerSegment 帧产出一片,时间戳连续。
 *
 * 纯内存运算、无 I/O、不持锁,可在任意线程使用(调用方自行串行化)。
 */
class ZmWebmMuxer
{
public:
    /**
     * @brief 构造(参数在生命周期内不变)
     *
     * @param sampleRate 采样率(Hz;本模块固定 48000)
     * @param channels   声道数(本模块固定 2)
     * @param preSkip    Opus 前置跳过量(采样数;由 libopus 的 lookahead 查询得到)
     */
    ZmWebmMuxer(uint32_t sampleRate, uint16_t channels, int preSkip);

    /**
     * @brief 构造初始化段
     *
     * 含 EBML 头、未知长度的 Segment(直播形态)、Info(时标 1ms)、Tracks
     * (CodecID=A_OPUS、CodecPrivate=OpusHead、CodecDelay、SeekPreRoll)。
     * 客户端把它作为第一段 appendBuffer 即可,无需解析。
     *
     * @return 初始化段字节
     */
    std::vector<uint8_t> BuildInitSegment() const;

    /**
     * @brief 追加一帧 Opus 数据;满一片时交出该簇
     *
     * @param frame Opus 包字节
     * @param len   字节数(0 或空指针即忽略)
     * @param ptsMs 该帧在采集时间轴上的毫秒位置(单调递增)
     * @return 非空 = 本片已满,返回簇字节;空 = 尚未满一片
     */
    std::vector<uint8_t> PushFrame(const uint8_t* frame, size_t len, uint64_t ptsMs);

    /// @brief 已累积的帧数是否够一片
    bool Ready() const { return m_pending.size() >= kFramesPerSegment; }

    /// @brief 分片时长(毫秒)
    uint32_t SegmentMs() const { return kFramesPerSegment * kFrameMs; }

    /// @brief 单帧时长(毫秒)
    static constexpr uint32_t kFrameMs = 20;
    /// @brief 一片包含的帧数(5 × 20ms = 100ms)
    ///
    /// 曾试过缩到 2 帧(40ms)以压低延迟,实测**没有收益**:媒体元素在数据太少时不肯进入
    /// 播放态,稳态缓冲余量压不下去(反而更难稳定),片长不是延迟的瓶颈 —— 客户端"与直播
    /// 边缘的距离"才是。
    static constexpr uint32_t kFramesPerSegment = 5;

private:
    /// @brief 构造 OpusHead(19 字节;映射族 0)
    std::vector<uint8_t> BuildOpusHead() const;
    /// @brief 构造 Tracks 元素(含单个音频轨道)
    std::vector<uint8_t> BuildTracks() const;

    uint32_t m_sampleRate = 48000;
    uint16_t m_channels = 2;
    int      m_preSkip = 0;
    /// 未满一片的帧:<采集时间轴毫秒, Opus 包>
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> m_pending;
};

#endif // ZM_UTIL_WEBM_MUXER_H
