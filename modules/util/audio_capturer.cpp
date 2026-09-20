#include "audio_capturer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 的 GUID 定义需 initguid.h 前置(仅本 TU)
#include <initguid.h>
#include <ksmedia.h>

#include <opus.h>

#include "zm_util_logger.h"

#include <chrono>
#include <cstring>
#include <vector>

#pragma comment(lib, "Ole32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
/// 日志前缀(便于在服务日志里过滤采集相关行)
constexpr const char* kLogTag = "[audio]";
/// 采集与编码的固定形态(本模块只产出这一种格式)
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 2;
/// 一帧 20ms
constexpr uint32_t kFrameMs = 20;
constexpr uint32_t kFrameSamples = kSampleRate / 1000 * kFrameMs;
/// Opus 单包上限(规格 1275B,留余量)
constexpr uint32_t kFrameMaxBytes = 1500;
/// 等待音频引擎事件的超时(超时分支负责探测设备并回调周期钩子)
constexpr DWORD kWaitMs = 200;

/**
 * @brief 把混音格式的包数据转成 int16 交错立体声
 *
 * 立体声源直通(保留左右声像);单声道/多声道源先混单(均值)再复制到左右,
 * 保证编码器输入恒为 2ch 交错。浮点输入限幅到 [-1, 1]。
 *
 * @param mixCh    混音声道数
 * @param mixBits  混音位深(16 或 32)
 * @param mixFloat 32bit 是否为 IEEE 浮点
 * @param src      源数据
 * @param frames   采样帧数
 * @param out      [out] 2 × frames 个 int16(交错)
 */
void ConvertToStereo16(int mixCh, int mixBits, bool mixFloat, const BYTE* src, UINT32 frames,
                       int16_t* out)
{
    const UINT32   ch  = static_cast<UINT32>(mixCh > 0 ? mixCh : 1);
    const int16_t* p16 = reinterpret_cast<const int16_t*>(src);
    const int32_t* p32 = reinterpret_cast<const int32_t*>(src);
    const float*   pf  = reinterpret_cast<const float*>(src);

    for (UINT32 i = 0; i < frames; ++i)
    {
        int16_t l = 0;
        int16_t r = 0;
        if (ch == 2)
        {
            if (mixFloat && mixBits == 32)
            {
                float fl = pf[i * 2];
                float fr = pf[i * 2 + 1];
                fl = fl > 1.0f ? 1.0f : (fl < -1.0f ? -1.0f : fl);
                fr = fr > 1.0f ? 1.0f : (fr < -1.0f ? -1.0f : fr);
                l = static_cast<int16_t>(fl * 32767.0f);
                r = static_cast<int16_t>(fr * 32767.0f);
            }
            else if (mixBits == 32)
            {
                l = static_cast<int16_t>(p32[i * 2] >> 16);
                r = static_cast<int16_t>(p32[i * 2 + 1] >> 16);
            }
            else
            {
                l = p16[i * 2];
                r = p16[i * 2 + 1];
            }
        }
        else if (mixFloat && mixBits == 32)
        {
            float m = 0.0f;
            for (UINT32 c = 0; c < ch; ++c)
                m += pf[i * ch + c];
            m /= static_cast<float>(ch);
            m = m > 1.0f ? 1.0f : (m < -1.0f ? -1.0f : m);
            l = r = static_cast<int16_t>(m * 32767.0f);
        }
        else if (mixBits == 32)
        {
            int32_t sum = 0;
            for (UINT32 c = 0; c < ch; ++c)
                sum += p32[i * ch + c] >> 16;
            l = r = static_cast<int16_t>(sum / static_cast<int32_t>(ch));
        }
        else
        {
            int32_t sum = 0;
            for (UINT32 c = 0; c < ch; ++c)
                sum += p16[i * ch + c];
            l = r = static_cast<int16_t>(sum / static_cast<int32_t>(ch));
        }
        out[i * 2]     = l;
        out[i * 2 + 1] = r;
    }
}

} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmAudioCapturer::ZmAudioCapturer() = default;

ZmAudioCapturer::~ZmAudioCapturer()
{
    StopAndJoin();
}

// ============================================================================
// Opus 前置跳过量
// ============================================================================
int ZmAudioCapturer::GetPreSkip()
{
    int          err = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !enc)
        return 0;
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    opus_encoder_destroy(enc);
    return static_cast<int>(lookahead);
}

// ============================================================================
// 启动:设备枚举 → 格式协商 → 引擎启动 → 编码器
// ============================================================================
bool ZmAudioCapturer::Start(FrameCb onFrame, TickCb onTick, DoneCb onDone, std::string& errMsg)
{
    StopAndJoin();   // 回收上一次已退出的线程(幂等)
    m_exit.store(false);

    // 本线程(调用方线程,通常是工作池线程)初始化为 MTA;
    // 与采集线程同属 MTA,创建的接口可跨这两个线程使用。长生命周期线程不 CoUninitialize
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
        DEFAULT_LOG_WARN("{} 调用线程非 MTA,采集接口将跨单元使用", kLogTag);
    else if (FAILED(hr))
    {
        errMsg = "COM 初始化失败";
        DEFAULT_LOG_WARN("{} CoInitializeEx 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
    {
        errMsg = "音频设备枚举器不可用";
        DEFAULT_LOG_WARN("{} 创建设备枚举器失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
    {
        errMsg = "无默认播放设备";
        DEFAULT_LOG_WARN("{} 无默认音频渲染设备 hr=0x{:08x}", kLogTag, (unsigned)hr);
        return false;
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr))
    {
        errMsg = "激活音频客户端失败";
        DEFAULT_LOG_WARN("{} 激活 IAudioClient 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
        return false;
    }

    WAVEFORMATEX* mixFmt = nullptr;
    hr = client->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt)
    {
        errMsg = "读取音频格式失败";
        DEFAULT_LOG_WARN("{} GetMixFormat 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
        return false;
    }

    WAVEFORMATEX* useFmt = mixFmt;
    WAVEFORMATEX* cand48 = nullptr;   // 48kHz 命中候选(堆副本,自行释放)
    DWORD         useFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE        hEvent = nullptr;
    bool          clientReady = false;   // 引擎已初始化且格式已校验
    int           mixCh = 0;
    int           mixBits = 0;
    bool          mixFloat = false;

    // ── 非 48kHz 混音设备(44.1k/96k 声卡、HDMI 输出等):先问引擎能否改用 48kHz,
    //    都不行再请引擎在客户端格式与混音格式之间插入采样率转换 ──
    if (mixFmt->nSamplesPerSec != kSampleRate)
    {
        std::vector<BYTE> c0buf;
        WAVEFORMATEX      pcm2x16{};
        WAVEFORMATEX      pcm1x16{};
        WAVEFORMATEXTENSIBLE f32x2{};
        struct Cand
        {
            const WAVEFORMATEX* fmt;
            const char*         desc;
        };
        std::vector<Cand> cands;
        cands.reserve(4);

        const size_t mixFmtSize = sizeof(WAVEFORMATEX) + mixFmt->cbSize;
        if (mixFmtSize <= sizeof(WAVEFORMATEXTENSIBLE) && mixFmt->nChannels <= 2 &&
            (mixFmt->wBitsPerSample == 16 || mixFmt->wBitsPerSample == 32))
        {
            c0buf.assign(mixFmtSize, 0);
            std::memcpy(c0buf.data(), mixFmt, mixFmtSize);
            auto* c0 = reinterpret_cast<WAVEFORMATEX*>(c0buf.data());
            c0->nSamplesPerSec  = kSampleRate;
            c0->nBlockAlign     = static_cast<WORD>(c0->nChannels * c0->wBitsPerSample / 8);
            c0->nAvgBytesPerSec = kSampleRate * c0->nBlockAlign;
            cands.push_back({c0, "48kHz/混音声道位深"});
        }
        pcm2x16.wFormatTag      = WAVE_FORMAT_PCM;
        pcm2x16.nChannels       = 2;
        pcm2x16.nSamplesPerSec  = kSampleRate;
        pcm2x16.wBitsPerSample  = 16;
        pcm2x16.nBlockAlign     = 4;
        pcm2x16.nAvgBytesPerSec = kSampleRate * 4;
        cands.push_back({&pcm2x16, "48kHz/2ch/16bit"});
        f32x2.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        f32x2.Format.nChannels       = 2;
        f32x2.Format.nSamplesPerSec  = kSampleRate;
        f32x2.Format.wBitsPerSample  = 32;
        f32x2.Format.nBlockAlign     = 8;
        f32x2.Format.nAvgBytesPerSec = kSampleRate * 8;
        f32x2.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f32x2.Samples.wValidBitsPerSample = 32;
        f32x2.dwChannelMask               = 0x3;   // FRONT_LEFT | FRONT_RIGHT
        f32x2.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        cands.push_back({&f32x2.Format, "48kHz/2ch/32float"});
        pcm1x16.wFormatTag      = WAVE_FORMAT_PCM;
        pcm1x16.nChannels       = 1;
        pcm1x16.nSamplesPerSec  = kSampleRate;
        pcm1x16.wBitsPerSample  = 16;
        pcm1x16.nBlockAlign     = 2;
        pcm1x16.nAvgBytesPerSec = kSampleRate * 2;
        cands.push_back({&pcm1x16, "48kHz/1ch/16bit"});

        // 阶段 1:逐一询问引擎(命中即用)
        for (const auto& cand : cands)
        {
            WAVEFORMATEX* closest = nullptr;
            const HRESULT hIs = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, cand.fmt, &closest);
            if (closest)
            {
                CoTaskMemFree(closest);
                closest = nullptr;
            }
            if (hIs != S_OK)
            {
                DEFAULT_LOG_INFO("{} 48kHz 候选[{}] 不被引擎支持 hr=0x{:08x}", kLogTag,
                                 cand.desc, (unsigned)hIs);
                continue;
            }
            cand48 = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX) + cand.fmt->cbSize));
            if (!cand48)
                break;
            std::memcpy(cand48, cand.fmt, sizeof(WAVEFORMATEX) + cand.fmt->cbSize);
            useFmt = cand48;
            DEFAULT_LOG_INFO("{} 混音 {}Hz 协商为 48kHz 候选[{}] 成功", kLogTag,
                             mixFmt->nSamplesPerSec, cand.desc);
            break;
        }

        // 阶段 2:请引擎插入转换(AUTOCONVERTPCM;SRC_DEFAULT_QUALITY 提高人耳聆听质量)
        if (!cand48)
        {
            const DWORD convFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            for (const auto& cand : cands)
            {
                hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!hEvent)
                    break;
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, convFlags, 0, 0, cand.fmt, nullptr);
                if (SUCCEEDED(hr))
                    hr = client->SetEventHandle(hEvent);
                if (FAILED(hr))
                {
                    DEFAULT_LOG_INFO("{} 引擎转换候选[{}] 初始化失败 hr=0x{:08x}", kLogTag,
                                     cand.desc, (unsigned)hr);
                    CloseHandle(hEvent);
                    hEvent = nullptr;
                    if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
                        break;   // 前候选已占用引擎,后续候选必然失败
                    continue;
                }
                hr = client->GetService(IID_PPV_ARGS(&capture));
                if (FAILED(hr))
                {
                    DEFAULT_LOG_WARN("{} 引擎转换候选[{}] GetService 失败 hr=0x{:08x}",
                                     kLogTag, cand.desc, (unsigned)hr);
                    CloseHandle(hEvent);
                    hEvent = nullptr;
                    break;
                }
                mixCh    = cand.fmt->nChannels;
                mixBits  = cand.fmt->wBitsPerSample;
                mixFloat = false;
                if (cand.fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(cand.fmt);
                    mixFloat        = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                }
                clientReady = true;
                useFlags    = convFlags;
                DEFAULT_LOG_INFO("{} 引擎转换初始化成功:{}Hz 混音 → 48kHz[{}]", kLogTag,
                                 mixFmt->nSamplesPerSec, cand.desc);
                break;
            }
            if (!clientReady)
                DEFAULT_LOG_WARN("{} 引擎不支持把 {}Hz 混音转为 {}Hz", kLogTag,
                                 mixFmt->nSamplesPerSec, kSampleRate);
        }
    }

    // ── 常规路径(混音即 48kHz,或阶段 1 命中):校验 + 快照 + 初始化 ──
    if (!clientReady)
    {
        auto fail = [&](std::string msg) {
            if (cand48)
                CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            errMsg = std::move(msg);
            return false;
        };
        if (useFmt->nSamplesPerSec != kSampleRate)
        {
            DEFAULT_LOG_WARN("{} 不支持的混音采样率 {}Hz(仅支持 {}Hz;可在系统声音设置里改为 48000Hz)",
                             kLogTag, useFmt->nSamplesPerSec, kSampleRate);
            return fail("不支持的混音采样率");
        }
        if (useFmt->nChannels != 1 && useFmt->nChannels != 2)
        {
            DEFAULT_LOG_WARN("{} 不支持的声道数 {}", kLogTag, useFmt->nChannels);
            return fail("不支持的声道数");
        }
        if (useFmt->wBitsPerSample != 16 && useFmt->wBitsPerSample != 32)
        {
            DEFAULT_LOG_WARN("{} 不支持的位深 {}", kLogTag, useFmt->wBitsPerSample);
            return fail("不支持的位深");
        }
        mixCh    = useFmt->nChannels;
        mixBits  = useFmt->wBitsPerSample;
        mixFloat = false;
        if (useFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(useFmt);
            mixFloat        = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }

        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent)
            return fail("创建采集事件失败");
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, useFlags, 0, 0, useFmt, nullptr);
        if (FAILED(hr))
        {
            CloseHandle(hEvent);
            hEvent = nullptr;
            DEFAULT_LOG_WARN("{} IAudioClient::Initialize 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
            return fail("音频引擎初始化失败");
        }
        hr = client->SetEventHandle(hEvent);
        if (FAILED(hr))
        {
            CloseHandle(hEvent);
            hEvent = nullptr;
            DEFAULT_LOG_WARN("{} SetEventHandle 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
            return fail("音频事件绑定失败");
        }
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr))
        {
            CloseHandle(hEvent);
            hEvent = nullptr;
            DEFAULT_LOG_WARN("{} 获取 IAudioCaptureClient 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
            return fail("音频采集接口不可用");
        }
    }
    if (cand48)
        CoTaskMemFree(cand48);
    CoTaskMemFree(mixFmt);

    // ── Opus 编码器(定长帧,音乐取向) ──
    int          opusErr = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &opusErr);
    if (opusErr != OPUS_OK || !enc)
    {
        CloseHandle(hEvent);
        DEFAULT_LOG_WARN("{} opus_encoder_create 失败 err={}", kLogTag, opusErr);
        errMsg = "音频编码器初始化失败";
        return false;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));   // 定长帧,流式更稳
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    hr = client->Start();
    if (FAILED(hr))
    {
        opus_encoder_destroy(enc);
        CloseHandle(hEvent);
        DEFAULT_LOG_WARN("{} IAudioClient::Start 失败 hr=0x{:08x}", kLogTag, (unsigned)hr);
        errMsg = "音频引擎启动失败";
        return false;
    }

    m_client      = client.Detach();
    m_capture     = capture.Detach();
    m_encoder     = enc;
    m_event       = hEvent;
    m_mixChannels = mixCh;
    m_mixBits     = mixBits;
    m_mixFloat    = mixFloat;
    m_onFrame     = std::move(onFrame);
    m_onTick      = std::move(onTick);
    m_onDone      = std::move(onDone);
    m_running.store(true);
    m_thread = std::thread(&ZmAudioCapturer::ThreadMain, this);

    DEFAULT_LOG_INFO("{} 采集启动:{}Hz {}ch {}bit{}", kLogTag, kSampleRate, m_mixChannels,
                     m_mixBits, m_mixFloat ? " float" : "");
    return true;
}

// ============================================================================
// 采集线程:泵包 → 格式转换 → Opus 编码 → 帧回调
// ============================================================================
void ZmAudioCapturer::ThreadMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<int16_t> accum;
    accum.reserve(static_cast<size_t>(kFrameSamples) * kChannels * 2);
    std::vector<int16_t> pcm;
    std::vector<uint8_t> opusBuf(kFrameMaxBytes);
    uint64_t             ptsMs = 0;
    bool                 deviceLost = false;

    // 追加若干帧采样(交错立体声),凑满 20ms 即编码交付
    auto pushSamples = [&](const int16_t* interleaved, UINT32 frames) {
        for (UINT32 i = 0; i < frames; ++i)
        {
            accum.push_back(interleaved[i * 2]);
            accum.push_back(interleaved[i * 2 + 1]);
        }
        while (accum.size() >= static_cast<size_t>(kFrameSamples) * kChannels)
        {
            const int len = opus_encode(m_encoder, accum.data(), static_cast<int>(kFrameSamples),
                                        opusBuf.data(), static_cast<opus_int32>(opusBuf.size()));
            accum.erase(accum.begin(), accum.begin() + static_cast<size_t>(kFrameSamples) * kChannels);
            if (len > 0 && m_onFrame)
                m_onFrame(opusBuf.data(), static_cast<size_t>(len), ptsMs);
            ptsMs += kFrameMs;
        }
    };

    // 时间轴对齐:无论设备是否交付数据,都按真实时间产出音频(缺口用静音补齐)
    //
    // 空闲设备(整机没有任何声音在播)时 WASAPI 回环可能**一个包都不交付** —— 若就此
    // 空转,时间轴会停走,客户端拿不到分片,一路停在"未收到音频数据"。这里以真实时间
    // 为基准:该产多少采样就产多少,设备没给的部分用静音帧补上(20ms 一整帧地补)。
    auto clockStart = std::chrono::steady_clock::now();
    auto topUpSilence = [&]() {
        const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now() - clockStart)
                                                           .count());
        const uint64_t wantMs = elapsedMs;
        const uint64_t haveMs = ptsMs;   // 已产出的音频时长(每帧 20ms 递增)
        if (wantMs <= haveMs + kFrameMs)
            return;
        uint64_t deficitMs = wantMs - haveMs;
        while (deficitMs >= kFrameMs && !m_exit.load())
        {
            pcm.assign(static_cast<size_t>(kFrameSamples) * kChannels, 0);
            pushSamples(pcm.data(), kFrameSamples);
            deficitMs -= kFrameMs;
        }
    };

    while (!m_exit.load())
    {
        if (WaitForSingleObject(static_cast<HANDLE>(m_event.load()), kWaitMs) != WAIT_OBJECT_0)
        {
            // 超时:引擎未交付数据(可能设备已失效)→ 探测一次
            UINT32  probe = 0;
            HRESULT h     = m_capture->GetNextPacketSize(&probe);
            if (FAILED(h) && h != AUDCLNT_E_BUFFER_ERROR)
            {
                DEFAULT_LOG_WARN("{} 采集失败 hr=0x{:08x},停止采集", kLogTag, (unsigned)h);
                deviceLost = true;
                break;
            }
            // 空闲设备(整机没有任何声音在播)时引擎可能一个包都不交付:
            // 由下面的"按真实时间补齐"负责出声,这里只需继续走时间轴
            topUpSilence();
            if (m_onTick)
                m_onTick();
            continue;
        }

        UINT32  packetCount = 0;
        HRESULT hr          = m_capture->GetNextPacketSize(&packetCount);
        if (FAILED(hr) && hr != AUDCLNT_E_BUFFER_ERROR)
        {
            DEFAULT_LOG_WARN("{} 采集失败 hr=0x{:08x},停止采集", kLogTag, (unsigned)hr);
            deviceLost = true;
            break;
        }
        while (hr == S_OK && packetCount > 0 && !m_exit.load())
        {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            if (m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr) != S_OK)
                break;

            if (frames > 0)
            {
                // 静音包照常编码:播放端时间轴跟数据走,静音不产片会让它停在缓冲末尾
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                {
                    pcm.assign(static_cast<size_t>(frames) * kChannels, 0);
                    pushSamples(pcm.data(), frames);
                }
                else
                {
                    pcm.resize(static_cast<size_t>(frames) * kChannels);
                    ConvertToStereo16(m_mixChannels, m_mixBits, m_mixFloat, data, frames, pcm.data());
                    pushSamples(pcm.data(), frames);
                }
            }
            m_capture->ReleaseBuffer(frames);
            // 排空循环的通行写法:释放后重新查询包数
            hr = m_capture->GetNextPacketSize(&packetCount);
        }

        topUpSilence();
        if (m_onTick)
            m_onTick();
    }

    // ── 收尾:停引擎并释放资源(幂等) ──
    if (m_client)
    {
        m_client->Stop();
        m_client->Release();
        m_client = nullptr;
    }
    if (m_capture)
    {
        m_capture->Release();
        m_capture = nullptr;
    }
    if (m_encoder)
    {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
    if (void* ev = m_event.exchange(nullptr))
        CloseHandle(static_cast<HANDLE>(ev));
    m_running.store(false);

    DEFAULT_LOG_INFO("{} 采集停止({};音频设备已释放)", kLogTag, deviceLost ? "设备失效" : "按请求停止");
    CoUninitialize();

    if (m_onDone)
        m_onDone();
}

// ============================================================================
// 停止
// ============================================================================
void ZmAudioCapturer::RequestStop()
{
    m_exit.store(true);
    if (void* ev = m_event.load())
        SetEvent(static_cast<HANDLE>(ev));   // 唤醒等待中的采集线程,不必等到 200ms 超时
}

void ZmAudioCapturer::StopAndJoin()
{
    RequestStop();
    if (m_thread.joinable())
        m_thread.join();
}
