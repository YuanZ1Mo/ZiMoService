#include "webm_muxer.h"

#include <cstring>

// ============================================================================
// EBML 元素编号(WebM 规格;编号自带长度标记,按最小字节数写出即可)
// ============================================================================
namespace
{
constexpr uint32_t kIdEBML            = 0x1A45DFA3;
constexpr uint32_t kIdEBMLVersion     = 0x4286;
constexpr uint32_t kIdEBMLReadVersion = 0x42F7;
constexpr uint32_t kIdEBMLMaxIDLen    = 0x42F2;
constexpr uint32_t kIdEBMLMaxSizeLen  = 0x42F3;
constexpr uint32_t kIdDocType         = 0x4282;
constexpr uint32_t kIdDocTypeVersion  = 0x4287;
constexpr uint32_t kIdDocTypeReadVer  = 0x4285;

constexpr uint32_t kIdSegment    = 0x18538067;
constexpr uint32_t kIdInfo       = 0x1549A966;
constexpr uint32_t kIdTimecodeSc = 0x2AD7B1;
constexpr uint32_t kIdMuxingApp  = 0x4D80;
constexpr uint32_t kIdWritingApp = 0x5741;

constexpr uint32_t kIdTracks       = 0x1654AE6B;
constexpr uint32_t kIdTrackEntry   = 0xAE;
constexpr uint32_t kIdTrackNumber  = 0xD7;
constexpr uint32_t kIdTrackUID     = 0x73C5;
constexpr uint32_t kIdTrackType    = 0x83;
constexpr uint32_t kIdFlagDefault  = 0x88;
constexpr uint32_t kIdCodecID      = 0x86;
constexpr uint32_t kIdCodecPrivate = 0x63A2;
constexpr uint32_t kIdCodecDelay   = 0x56AA;
constexpr uint32_t kIdSeekPreRoll  = 0x56BB;
constexpr uint32_t kIdAudio        = 0xE1;
constexpr uint32_t kIdSamplingFreq = 0xB5;
constexpr uint32_t kIdChannels     = 0x9F;

constexpr uint32_t kIdCluster     = 0x1F43B675;
constexpr uint32_t kIdTimecode    = 0xE7;
constexpr uint32_t kIdSimpleBlock = 0xA3;

/// 音频轨道号(单轨固定 1)
constexpr uint64_t kTrackNumber = 1;
/// SimpleBlock 关键帧标志(音频帧全部带)
constexpr uint8_t kBlockFlagKeyframe = 0x80;
/// SeekPreRoll:80ms(纳秒;Opus 解码器预热窗口的通行取值)
constexpr uint64_t kSeekPreRollNs = 80000000ULL;

/// @brief 追加原始字节
void PutRaw(std::vector<uint8_t>& v, const void* p, size_t n)
{
    const auto* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}

/// @brief 写 EBML 元素编号(按数值所需的最小字节数)
void PutId(std::vector<uint8_t>& v, uint32_t id)
{
    const int w = (id < 0x100) ? 1 : (id < 0x10000) ? 2 : (id < 0x1000000) ? 3 : 4;
    for (int i = w - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((id >> (8 * i)) & 0xFF));
}

/// @brief 写尺寸 vint(值全 1 的形态保留给"未知长度",故可取上界为 2^(7w)-2)
void PutVint(std::vector<uint8_t>& v, uint64_t size)
{
    int w = 1;
    while (w < 8 && size >= (1ULL << (7 * w)) - 1)
        ++w;
    for (int i = w - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((size >> (8 * i)) & 0xFF));
    v[v.size() - static_cast<size_t>(w)] |= static_cast<uint8_t>(1u << (8 - w));
}

/// @brief EBML 无符号整数(大端,最小字节数)
std::vector<uint8_t> UIntBE(uint64_t v)
{
    std::vector<uint8_t> out;
    int w = 1;
    while (w < 8 && v >= (1ULL << (8 * w)))
        ++w;
    for (int i = w - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    return out;
}

/// @brief 整数元素
std::vector<uint8_t> ElemUInt(uint32_t id, uint64_t v)
{
    std::vector<uint8_t> out;
    PutId(out, id);
    const std::vector<uint8_t> body = UIntBE(v);
    PutVint(out, body.size());
    PutRaw(out, body.data(), body.size());
    return out;
}

/// @brief 字符串元素(ASCII)
std::vector<uint8_t> ElemStr(uint32_t id, const char* s)
{
    std::vector<uint8_t> out;
    PutId(out, id);
    const size_t n = std::strlen(s);
    PutVint(out, n);
    PutRaw(out, s, n);
    return out;
}

/// @brief 二进制元素
std::vector<uint8_t> ElemBin(uint32_t id, const uint8_t* p, size_t n)
{
    std::vector<uint8_t> out;
    PutId(out, id);
    PutVint(out, n);
    PutRaw(out, p, n);
    return out;
}

/// @brief 容器元素(编号 + 尺寸 + 载荷)
std::vector<uint8_t> Elem(uint32_t id, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    PutId(out, id);
    PutVint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// @brief 追加一个容器元素
void AppendElem(std::vector<uint8_t>& dst, uint32_t id, const std::vector<uint8_t>& payload)
{
    const std::vector<uint8_t> e = Elem(id, payload);
    dst.insert(dst.end(), e.begin(), e.end());
}

/// @brief 追加一个整数元素
void AppendUInt(std::vector<uint8_t>& dst, uint32_t id, uint64_t v)
{
    const std::vector<uint8_t> e = ElemUInt(id, v);
    dst.insert(dst.end(), e.begin(), e.end());
}

/// @brief 4 字节大端浮点载荷(SamplingFrequency 用)
std::vector<uint8_t> ElemFloat32Body(float f)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return {static_cast<uint8_t>(bits >> 24), static_cast<uint8_t>(bits >> 16),
            static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)};
}

} // namespace

// ============================================================================
// 构造
// ============================================================================
ZmWebmMuxer::ZmWebmMuxer(uint32_t sampleRate, uint16_t channels, int preSkip)
    : m_sampleRate(sampleRate), m_channels(channels), m_preSkip(preSkip < 0 ? 0 : preSkip)
{
    m_pending.reserve(kFramesPerSegment);
}

// ============================================================================
// OpusHead(CodecPrivate):映射族 0 的固定 19 字节布局
//   "OpusHead" + 版本 + 声道数 + preSkip(小端 u16) + 输入采样率(小端 u32)
//   + 输出增益(小端 i16) + 映射族
// ============================================================================
std::vector<uint8_t> ZmWebmMuxer::BuildOpusHead() const
{
    std::vector<uint8_t> out;
    const char magic[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    PutRaw(out, magic, sizeof(magic));
    out.push_back(1);                                    // 版本
    out.push_back(static_cast<uint8_t>(m_channels));     // 声道数
    out.push_back(static_cast<uint8_t>(m_preSkip & 0xFF));
    out.push_back(static_cast<uint8_t>((m_preSkip >> 8) & 0xFF));
    // 输入采样率(小端 u32)
    out.push_back(static_cast<uint8_t>(m_sampleRate & 0xFF));
    out.push_back(static_cast<uint8_t>((m_sampleRate >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((m_sampleRate >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((m_sampleRate >> 24) & 0xFF));
    out.push_back(0);                                    // 输出增益 = 0(小端 i16)
    out.push_back(0);
    out.push_back(0);                                    // 映射族 0(无声道映射表)
    return out;
}

// ============================================================================
// Tracks:单条 A_OPUS 音频轨
// ============================================================================
std::vector<uint8_t> ZmWebmMuxer::BuildTracks() const
{
    std::vector<uint8_t> audio;
    AppendElem(audio, kIdSamplingFreq, ElemFloat32Body(static_cast<float>(m_sampleRate)));
    AppendUInt(audio, kIdChannels, m_channels);

    const std::vector<uint8_t> head = BuildOpusHead();

    std::vector<uint8_t> track;
    AppendUInt(track, kIdTrackNumber, kTrackNumber);
    AppendUInt(track, kIdTrackUID, kTrackNumber);
    AppendUInt(track, kIdTrackType, 2);        // 2 = 音频
    AppendUInt(track, kIdFlagDefault, 1);
    AppendElem(track, kIdCodecID, [] {
        std::vector<uint8_t> s;
        const char*          id = "A_OPUS";
        s.assign(id, id + std::strlen(id));
        return s;
    }());
    AppendElem(track, kIdCodecPrivate, head);
    // CodecDelay = preSkip 折算成纳秒(编码器前置跳过量,解码端据此对齐时间轴)
    const uint64_t delayNs = m_sampleRate ? (static_cast<uint64_t>(m_preSkip) * 1000000000ULL / m_sampleRate) : 0;
    AppendUInt(track, kIdCodecDelay, delayNs);
    AppendUInt(track, kIdSeekPreRoll, kSeekPreRollNs);
    AppendElem(track, kIdAudio, audio);

    std::vector<uint8_t> tracks;
    AppendElem(tracks, kIdTrackEntry, track);
    return tracks;
}

// ============================================================================
// 初始化段
// ============================================================================
std::vector<uint8_t> ZmWebmMuxer::BuildInitSegment() const
{
    // EBML 头
    std::vector<uint8_t> ebml;
    AppendUInt(ebml, kIdEBMLVersion, 1);
    AppendUInt(ebml, kIdEBMLReadVersion, 1);
    AppendUInt(ebml, kIdEBMLMaxIDLen, 4);
    AppendUInt(ebml, kIdEBMLMaxSizeLen, 8);
    {
        const std::vector<uint8_t> v = ElemStr(kIdDocType, "webm");
        ebml.insert(ebml.end(), v.begin(), v.end());
    }
    AppendUInt(ebml, kIdDocTypeVersion, 4);
    AppendUInt(ebml, kIdDocTypeReadVer, 2);

    std::vector<uint8_t> out = Elem(kIdEBML, ebml);

    // Segment:直播形态用"未知长度"(8 字节 vint 全 1),后续簇可无限追加
    PutId(out, kIdSegment);
    out.push_back(0x01);
    for (int i = 0; i < 7; ++i)
        out.push_back(0xFF);

    // Info:时标 1ms;不写 Duration(直播无总长)
    std::vector<uint8_t> info;
    AppendUInt(info, kIdTimecodeSc, 1000000);
    {
        const std::vector<uint8_t> mux = ElemStr(kIdMuxingApp, "ZiMoService");
        info.insert(info.end(), mux.begin(), mux.end());
        const std::vector<uint8_t> wr = ElemStr(kIdWritingApp, "ZiMoService");
        info.insert(info.end(), wr.begin(), wr.end());
    }
    AppendElem(out, kIdInfo, info);

    // Tracks
    AppendElem(out, kIdTracks, BuildTracks());
    return out;
}

// ============================================================================
// 追加一帧:满一片则产出簇
// ============================================================================
std::vector<uint8_t> ZmWebmMuxer::PushFrame(const uint8_t* frame, size_t len, uint64_t ptsMs)
{
    if (!frame || len == 0 || len > 0xFFFF)
        return {};

    m_pending.emplace_back(ptsMs, std::vector<uint8_t>(frame, frame + len));
    if (m_pending.size() < kFramesPerSegment)
        return {};

    // 簇时标取本片首帧的绝对毫秒位置;块内用相对时标(i16,一片内不超过 100ms)
    const uint64_t baseMs = m_pending.front().first;

    std::vector<uint8_t> payload;
    AppendUInt(payload, kIdTimecode, baseMs);
    for (const auto& [pts, data] : m_pending)
    {
        std::vector<uint8_t> block;
        PutVint(block, kTrackNumber);
        const int64_t rel = static_cast<int64_t>(pts) - static_cast<int64_t>(baseMs);
        const int16_t rel16 = static_cast<int16_t>(rel < -32768 ? -32768 : (rel > 32767 ? 32767 : rel));
        block.push_back(static_cast<uint8_t>((rel16 >> 8) & 0xFF));
        block.push_back(static_cast<uint8_t>(rel16 & 0xFF));
        block.push_back(kBlockFlagKeyframe);
        PutRaw(block, data.data(), data.size());
        AppendElem(payload, kIdSimpleBlock, block);
    }
    m_pending.clear();
    return Elem(kIdCluster, payload);
}
