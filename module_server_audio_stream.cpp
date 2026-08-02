#include "module_server_audio_stream.h"

#include "zm_net_http.h"         // ZmHttpdTask(发送线程流式接口,直接 task 访问)
#include "zm_net_req_loop.h"     // ZmReqLoop + REQ_LOOP_SIG_DONE(流收尾投递)
#include "zm_logger.h"

#include <opus.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 的 GUID 定义需 initguid.h 前置(仅本 TU)
#include <initguid.h>
#include <ksmedia.h>

#include <cstring>

#pragma comment(lib, "Ole32.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// 内部工具
// ============================================================================
namespace {

/** @brief 小端写入 32 位无符号 */
void WriteLE32(BYTE* p, uint32_t v)
{
    p[0] = (BYTE)(v & 0xFF);
    p[1] = (BYTE)((v >> 8) & 0xFF);
    p[2] = (BYTE)((v >> 16) & 0xFF);
    p[3] = (BYTE)((v >> 24) & 0xFF);
}

/**
 * @brief 将混音格式快照对应的包数据转换为双声道交错 int16
 * 立体声源直通(保留左右声像);单声道/多声道源先混单(均值)再复制到 L/R,
 * 保证 Opus 编码器输入恒为 2ch 交错
 */
void ConvertToStereo16(int mixCh, int mixBits, bool mixFloat,
                       const BYTE* src, UINT32 frames, int16_t* out)
{
    const UINT32 ch = (UINT32)(mixCh > 0 ? mixCh : 1);
    const int16_t* p16 = reinterpret_cast<const int16_t*>(src);
    const int32_t* p32 = reinterpret_cast<const int32_t*>(src);
    const float*   pf  = reinterpret_cast<const float*>(src);

    for (UINT32 i = 0; i < frames; ++i)
    {
        int16_t l, r;
        if (ch == 2)
        {
            // 立体声直通
            if (mixFloat && mixBits == 32)
            {
                float fl = pf[i * 2], fr = pf[i * 2 + 1];
                if (fl > 1.0f) fl = 1.0f; else if (fl < -1.0f) fl = -1.0f;
                if (fr > 1.0f) fr = 1.0f; else if (fr < -1.0f) fr = -1.0f;
                l = (int16_t)(fl * 32767.0f);
                r = (int16_t)(fr * 32767.0f);
            }
            else if (mixBits == 32)   // 32-bit PCM
            {
                l = (int16_t)(p32[i * 2] >> 16);
                r = (int16_t)(p32[i * 2 + 1] >> 16);
            }
            else   // 16-bit PCM
            {
                l = p16[i * 2];
                r = p16[i * 2 + 1];
            }
        }
        else
        {
            // 非立体声:混单(均值)后复制到 L/R
            if (mixFloat && mixBits == 32)
            {
                float m = 0.0f;
                for (UINT32 c = 0; c < ch; ++c) m += pf[i * ch + c];
                m /= (float)ch;
                if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;
                l = r = (int16_t)(m * 32767.0f);
            }
            else if (mixBits == 32)
            {
                int32_t sum = 0;
                for (UINT32 c = 0; c < ch; ++c) sum += p32[i * ch + c] >> 16;
                l = r = (int16_t)(sum / (int32_t)ch);
            }
            else
            {
                int32_t sum = 0;
                for (UINT32 c = 0; c < ch; ++c) sum += p16[i * ch + c];
                l = r = (int16_t)(sum / (int32_t)ch);
            }
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
}

} // namespace

// ============================================================================
// Subscriber 析构(释放 WebM 混流器)
// ============================================================================

// ============================================================================
// 析构
// ============================================================================

void ServerAudioStreamModule::SetTasksGone()
{
    // 幂等;须在 NetDock 析构前由 ServiceCenter::OnStop 调用(见类注释)
    std::lock_guard lock(m_mutex);
    m_tasksGone = true;
}

ServerAudioStreamModule::~ServerAudioStreamModule()
{
    // 先标记 task/loop 即将失效(OnStop 通常已提前置位,此处兜底):
    // 发送线程清理时将跳过 EndStreamReply/PostToLoop(防止访问已释放对象)
    SetTasksGone();

    StopCapture();

    // 停止并回收所有发送线程(服务停止路径:NetDock 已先销毁,task/loop 已失效,
    // m_tasksGone 已置位,发送线程不会访问流对象)。
    // 发送线程退出时会把自身 Subscriber 移交 m_zombies(锁内),因此此处只 join
    // 活跃线程,不 delete——统一由 ReapZombies 回收(发送线程绝不能 delete 自身:
    // 其 std::thread 成员仍 joinable,析构会 std::terminate → 进程崩溃)。
    std::vector<Subscriber*> subs;
    {
        std::lock_guard lock(m_mutex);
        for (auto& [task, sub] : m_subscribers)
        {
            {
                std::lock_guard lk(sub->mtx);
                sub->stopped = true;
            }
            sub->cv.notify_all();
            subs.push_back(sub);
        }
        m_subscribers.clear();
    }
    for (auto* sub : subs)
    {
        if (sub->thread.joinable())
            sub->thread.join();
        // 注意:不 delete——发送线程退出时已将其移交 m_zombies,由 ReapZombies 回收
    }
    ReapZombies();
}

// ============================================================================
// 回收已退出发送线程的 Subscriber
// ============================================================================

void ServerAudioStreamModule::ReapZombies()
{
    // 循环回收:发送线程可能在本函数执行期间陆续移交(某轮取空即止);
    // 极少数极端时序下(发送线程恰在最后一轮取空后移交)进程退出时可能遗留
    // 单个对象,无功能影响。
    for (;;)
    {
        std::vector<Subscriber*> zombies;
        {
            std::lock_guard lock(m_mutex);
            zombies.swap(m_zombies);
        }
        if (zombies.empty())
            break;
        for (auto* sub : zombies)
        {
            if (sub->thread.joinable())
                sub->thread.join();
            delete sub;
        }
    }
}

// ============================================================================
// 订阅(触发按需启动采集)
// ============================================================================

bool ServerAudioStreamModule::Subscribe(ZmHttpdTask* task, ZmReqLoop* loop)
{
    ReapZombies();   // 先回收已退出发送线程的 Subscriber(join 不持锁)

    std::lock_guard lock(m_mutex);

    if (m_capturing)
    {
        // 采集线程正在收尾(停止标志已置)→ 瞬态失败,客户端重连后重新订阅
        if (m_captureExit.load())
            return false;
    }
    // 采集未运行 → 尝试启动;失败说明无可用音频设备
    else if (!TryStartCaptureLocked())
    {
        return false;
    }

    if (m_subscribers.count(task))
        return true;   // 防御:同一连接重复订阅

    auto* sub = new Subscriber();
    sub->task = task;
    sub->loop = loop;
    m_subscribers[task] = sub;
    sub->thread = std::thread(&ServerAudioStreamModule::SenderThreadMain, this, sub);
    return true;
}

bool ServerAudioStreamModule::IsSubscriberAlive(ZmHttpdTask* task) const
{
    std::lock_guard lock(m_mutex);
    return m_subscribers.count(task) > 0;
}

// ============================================================================
// 采集管线启动(要求已持有 m_mutex)
// ============================================================================

bool ServerAudioStreamModule::TryStartCaptureLocked()
{
    // 本线程(A 线程)初始化 COM 为 MTA;长生命周期线程,不主动 CoUninitialize
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
        DEFAULT_LOG_WARN("[audio] CoInitializeEx 返回 RPC_E_CHANGED_MODE(A 线程非 MTA,跨单元使用接口)");
    else if (FAILED(hr))
    {
        DEFAULT_LOG_WARN("[audio] CoInitializeEx 失败 hr=0x{:08x}", (unsigned)hr);
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
    {
        DEFAULT_LOG_WARN("[audio] 创建设备枚举器失败 hr=0x{:08x}", (unsigned)hr);
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
    {
        DEFAULT_LOG_WARN("[audio] 无默认音频渲染设备 hr=0x{:08x}", (unsigned)hr);
        return false;
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr))
    {
        DEFAULT_LOG_WARN("[audio] 激活 IAudioClient 失败 hr=0x{:08x}", (unsigned)hr);
        return false;
    }

    // 混音格式:要求 48kHz / 1-2 声道 / 16 或 32bit(现实设备几乎全为 48kHz)
    WAVEFORMATEX* mixFmt = nullptr;
    hr = client->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt)
    {
        DEFAULT_LOG_WARN("[audio] GetMixFormat 失败 hr=0x{:08x}", (unsigned)hr);
        return false;
    }

    // 非 48kHz 混音设备(如 44.1k/96k 声卡、HDMI 输出):共享模式引擎默认只支持与
    // 混音格式同采样率的格式,但仍按 WASAPI 惯例先询问引擎是否支持 48kHz(部分
    // 设备/APO 可转换);全部不支持时,再按 SDK 文档以 AUTOCONVERTPCM 标志请求
    // 引擎在客户端格式与混音格式之间插入采样率转换器(附 SRC_DEFAULT_QUALITY
    // 提升人耳聆听质量)。候选 0 保持混音声道/位深仅改采样率;候选 1/2/3 为常见
    // 48k 配置(2ch/16bit、2ch/32float、1ch/16bit)。全部失败则按原逻辑拒绝。
    WAVEFORMATEX* useFmt = mixFmt;
    WAVEFORMATEX* cand48 = nullptr;   // 48kHz 命中候选(堆上副本,需自行释放)
    DWORD useFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE hEvent = nullptr;
    bool clientReady = false;         // 引擎转换阶段已完成初始化(格式已校验)

    if (mixFmt->nSamplesPerSec != kAudioSampleRate)
    {
        // ── 构建候选格式(栈上数组,生命周期覆盖 Initialize;全部满足 1-2ch/16-32bit) ──
        std::vector<BYTE> c0buf;
        WAVEFORMATEX pcm2x16{};
        WAVEFORMATEX pcm1x16{};
        WAVEFORMATEXTENSIBLE f32f2{};
        struct Cand { const WAVEFORMATEX* fmt; const char* desc; };
        std::vector<Cand> cands;
        cands.reserve(4);

        const size_t mixFmtSize = sizeof(WAVEFORMATEX) + mixFmt->cbSize;
        if (mixFmtSize <= sizeof(WAVEFORMATEXTENSIBLE) &&
            mixFmt->nChannels <= 2 &&
            (mixFmt->wBitsPerSample == 16 || mixFmt->wBitsPerSample == 32))
        {
            c0buf.assign(mixFmtSize, 0);
            std::memcpy(c0buf.data(), mixFmt, mixFmtSize);
            auto* c0 = reinterpret_cast<WAVEFORMATEX*>(c0buf.data());
            c0->nSamplesPerSec    = kAudioSampleRate;
            c0->nBlockAlign       = (WORD)(c0->nChannels * c0->wBitsPerSample / 8);
            c0->nAvgBytesPerSec   = kAudioSampleRate * c0->nBlockAlign;
            cands.push_back({c0, "48kHz/混音声道位深"});
        }
        pcm2x16.wFormatTag      = WAVE_FORMAT_PCM;
        pcm2x16.nChannels       = 2;
        pcm2x16.nSamplesPerSec  = kAudioSampleRate;
        pcm2x16.wBitsPerSample  = 16;
        pcm2x16.nBlockAlign     = 4;
        pcm2x16.nAvgBytesPerSec = kAudioSampleRate * 4;
        cands.push_back({&pcm2x16, "48kHz/2ch/16bit"});
        f32f2.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        f32f2.Format.nChannels       = 2;
        f32f2.Format.nSamplesPerSec  = kAudioSampleRate;
        f32f2.Format.wBitsPerSample  = 32;
        f32f2.Format.nBlockAlign     = 8;
        f32f2.Format.nAvgBytesPerSec = kAudioSampleRate * 8;
        f32f2.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f32f2.Samples.wValidBitsPerSample = 32;
        f32f2.dwChannelMask         = 0x3;   // FRONT_LEFT | FRONT_RIGHT
        f32f2.SubFormat             = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        cands.push_back({&f32f2.Format, "48kHz/2ch/32float"});
        pcm1x16.wFormatTag      = WAVE_FORMAT_PCM;
        pcm1x16.nChannels       = 1;
        pcm1x16.nSamplesPerSec  = kAudioSampleRate;
        pcm1x16.wBitsPerSample  = 16;
        pcm1x16.nBlockAlign     = 2;
        pcm1x16.nAvgBytesPerSec = kAudioSampleRate * 2;
        cands.push_back({&pcm1x16, "48kHz/1ch/16bit"});

        // ── 阶段 1:逐一询问引擎(命中即用,后续候选不再尝试) ──
        for (const auto& cand : cands)
        {
            WAVEFORMATEX* closest = nullptr;
            HRESULT hIs = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                                    cand.fmt, &closest);
            if (closest) { CoTaskMemFree(closest); closest = nullptr; }
            if (hIs != S_OK)
            {
                DEFAULT_LOG_INFO("[audio] 48kHz 候选[{}] 不被引擎支持 hr=0x{:08x}",
                                 cand.desc, (unsigned)hIs);
                continue;
            }
            cand48 = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(
                         sizeof(WAVEFORMATEX) + cand.fmt->cbSize));
            if (!cand48)
                break;
            std::memcpy(cand48, cand.fmt, sizeof(WAVEFORMATEX) + cand.fmt->cbSize);
            useFmt = cand48;
            DEFAULT_LOG_INFO("[audio] 混音 {}Hz 协商为 48kHz 候选[{}] 成功",
                             mixFmt->nSamplesPerSec, cand.desc);
            break;
        }

        // ── 阶段 2:引擎采样率转换(AUTOCONVERTPCM 使引擎在客户端格式与混音
        // 格式之间插入声道矩阵与采样率转换器;SRC_DEFAULT_QUALITY 提高转换质量) ──
        if (!cand48)
        {
            const DWORD convFlags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            for (const auto& cand : cands)
            {
                hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!hEvent)
                    break;
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, convFlags,
                                        0, 0, cand.fmt, nullptr);
                if (SUCCEEDED(hr))
                    hr = client->SetEventHandle(hEvent);
                if (FAILED(hr))
                {
                    DEFAULT_LOG_INFO("[audio] 引擎转换候选[{}] 初始化失败 hr=0x{:08x}",
                                     cand.desc, (unsigned)hr);
                    CloseHandle(hEvent);
                    hEvent = nullptr;
                    if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
                        break;   // 前候选已初始化但后置步骤失败 → 后续候选必然失败
                    continue;
                }
                hr = client->GetService(IID_PPV_ARGS(&capture));
                if (FAILED(hr))
                {
                    DEFAULT_LOG_WARN("[audio] 引擎转换候选[{}] GetService 失败 hr=0x{:08x}",
                                     cand.desc, (unsigned)hr);
                    CloseHandle(hEvent);
                    hEvent = nullptr;
                    break;
                }
                // 命中:引擎以 48kHz 交付转换后的混音,快照采集格式
                m_mixChannels = cand.fmt->nChannels;
                m_mixBits     = cand.fmt->wBitsPerSample;
                m_mixFloat    = false;
                if (cand.fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                {
                    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(cand.fmt);
                    m_mixFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                }
                clientReady = true;
                useFlags = convFlags;
                DEFAULT_LOG_INFO("[audio] 引擎转换初始化成功:{}Hz 混音 → 48kHz[{}]",
                                 mixFmt->nSamplesPerSec, cand.desc);
                break;
            }
            if (!clientReady)
            {
                DEFAULT_LOG_WARN("[audio] 引擎不支持将 {}Hz 混音转为 {}Hz(共享模式不做采样率转换)",
                                 mixFmt->nSamplesPerSec, kAudioSampleRate);
            }
        }
    }

    // 常规路径(混音格式即 48kHz,或阶段 1 命中):校验 + 快照 + 初始化。
    // 阶段 2 成功时 clientReady=true,格式已按构建约束校验并快照。
    if (!clientReady)
    {
        if (useFmt->nSamplesPerSec != kAudioSampleRate)
        {
            DEFAULT_LOG_WARN("[audio] 不支持的混音采样率 {}Hz(仅支持 {}Hz;"
                             "非 48kHz 设备请在系统声音设置中将默认格式改为 48000Hz)",
                             useFmt->nSamplesPerSec, kAudioSampleRate);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        if (useFmt->nChannels != 1 && useFmt->nChannels != 2)
        {
            DEFAULT_LOG_WARN("[audio] 不支持的声道数 {}", useFmt->nChannels);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        if (useFmt->wBitsPerSample != 16 && useFmt->wBitsPerSample != 32)
        {
            DEFAULT_LOG_WARN("[audio] 不支持的位深 {}", useFmt->wBitsPerSample);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        m_mixChannels = useFmt->nChannels;
        m_mixBits     = useFmt->wBitsPerSample;
        m_mixFloat    = false;
        if (useFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(useFmt);
            m_mixFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }

        // 事件驱动采集
        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent)
        {
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, useFlags,
                                0, 0, useFmt, nullptr);
        if (FAILED(hr))
        {
            DEFAULT_LOG_WARN("[audio] IAudioClient::Initialize 失败 hr=0x{:08x}", (unsigned)hr);
            CloseHandle(hEvent);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        hr = client->SetEventHandle(hEvent);
        if (FAILED(hr))
        {
            DEFAULT_LOG_WARN("[audio] SetEventHandle 失败 hr=0x{:08x}", (unsigned)hr);
            CloseHandle(hEvent);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr))
        {
            DEFAULT_LOG_WARN("[audio] 获取 IAudioCaptureClient 失败 hr=0x{:08x}", (unsigned)hr);
            CloseHandle(hEvent);
            if (cand48) CoTaskMemFree(cand48);
            CoTaskMemFree(mixFmt);
            return false;
        }
    }
    if (cand48) CoTaskMemFree(cand48);
    CoTaskMemFree(mixFmt);

    // Opus 编码器
    int opusErr = OPUS_OK;
    auto* enc = opus_encoder_create(kAudioSampleRate, kAudioChannels,
                                    OPUS_APPLICATION_AUDIO, &opusErr);
    if (opusErr != OPUS_OK)
    {
        DEFAULT_LOG_WARN("[audio] opus_encoder_create 失败 err={}", opusErr);
        CloseHandle(hEvent);
        return false;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kAudioBitrate));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));                    // 定长帧,流式更稳
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    // 上一个采集线程可能已自停但未回收(空闲停采/设备失效路径只发信号不 join):
    // 先回收线程句柄,避免对 joinable 线程重新赋值触发 std::terminate。
    // m_capturing==false 保证旧线程已完成锁内收尾,此处 join 有界且无锁竞争
    if (m_captureThread.joinable())
        m_captureThread.join();

    // 转存成员(采集线程使用);采集线程退出时在锁内释放并置空
    m_opusEncoder   = enc;
    m_audioClient   = client.Detach();
    m_captureClient = capture.Detach();
    m_captureEvent  = hEvent;
    m_captureExit.store(false);
    m_captureThread = std::thread(&ServerAudioStreamModule::CaptureThreadMain, this);
    m_capturing = true;

    DEFAULT_LOG_INFO("[audio] 采集启动:{}Hz {}ch {}bit{}",
                     kAudioSampleRate, m_mixChannels, m_mixBits, m_mixFloat ? " float" : "");
    return true;
}

// ============================================================================
// 采集线程:WASAPI loopback → Opus 编码 → 分发
// ============================================================================

void ServerAudioStreamModule::CaptureThreadMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto* client   = static_cast<IAudioClient*>(m_audioClient);
    auto* capture  = static_cast<IAudioCaptureClient*>(m_captureClient);
    HANDLE hEvent  = static_cast<HANDLE>(m_captureEvent);
    auto* enc      = static_cast<OpusEncoder*>(m_opusEncoder);

    std::vector<int16_t> accum;                 // 未凑满一帧的采样
    accum.reserve(kAudioFrameSamples * kAudioChannels * 2);
    std::vector<int16_t> pcmBuf(kAudioFrameSamples * kAudioChannels);   // 交错双声道
    std::vector<BYTE> opusBuf(kAudioFrameMaxLen);

    // Start 失败(如设备刚失效)→ 记日志并直接走收尾路径
    HRESULT hrStart = client->Start();
    if (FAILED(hrStart))
        DEFAULT_LOG_WARN("[audio] IAudioClient::Start 失败 hr=0x{:08x}", (unsigned)hrStart);

    while (SUCCEEDED(hrStart))
    {
        if (WaitForSingleObject(hEvent, 200) != WAIT_OBJECT_0)
        {
            // 超时(事件停止触发,可能设备失效)→ 探测设备状态
            UINT32 probeCount = 0;
            HRESULT hProbe = capture->GetNextPacketSize(&probeCount);
            if (FAILED(hProbe) && hProbe != AUDCLNT_E_BUFFER_ERROR)
            {
                DEFAULT_LOG_WARN("[audio] 采集失败 hr=0x{:08x},关闭所有订阅者流", (unsigned)hProbe);
                break;
            }
            // 无订阅者 → 自行停止(状态机回 IDLE);停止标志须与判断同锁,
            // 防止期间新订阅者被接受后立即被收尾终止
            bool shouldStop = m_captureExit.load();
            if (!shouldStop)
            {
                std::lock_guard lock(m_mutex);
                shouldStop = m_subscribers.empty();
                if (shouldStop)
                    m_captureExit.store(true);
            }
            if (shouldStop)
                break;
            continue;
        }

        UINT32 packetCount = 0;
        HRESULT hbr = capture->GetNextPacketSize(&packetCount);
        if (FAILED(hbr) && hbr != AUDCLNT_E_BUFFER_ERROR)
        {
            // 设备失效(拔出/格式变更)→ 通知所有订阅者结束流,回 IDLE
            DEFAULT_LOG_WARN("[audio] 采集失败 hr=0x{:08x},关闭所有订阅者流", (unsigned)hbr);
            break;
        }
        while (hbr == S_OK && packetCount > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr) != S_OK)
                break;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data && frames > 0)
            {
                pcmBuf.resize((size_t)frames * 2);
                ConvertToStereo16(m_mixChannels, m_mixBits, m_mixFloat,
                                  data, frames, pcmBuf.data());
                for (UINT32 i = 0; i < frames; ++i)
                {
                    accum.push_back(pcmBuf[(size_t)i * 2]);
                    accum.push_back(pcmBuf[(size_t)i * 2 + 1]);
                    if (accum.size() >= kAudioFrameSamples * kAudioChannels)
                    {
                        int len = opus_encode(enc, accum.data(), (int)kAudioFrameSamples,
                                              opusBuf.data(), (opus_int32)opusBuf.size());
                        accum.erase(accum.begin(),
                                    accum.begin() + kAudioFrameSamples * kAudioChannels);
                        if (len > 0)
                            DispatchFrame(opusBuf.data(), len);
                    }
                }
            }
            capture->ReleaseBuffer(frames);
            // 排空循环标准模式:释放后重新查询包数
            hbr = capture->GetNextPacketSize(&packetCount);
        }

        // 有声播放时事件持续触发、永不走超时分支——此处必须同样检查停止信号,
        // 否则"无订阅者自停"与 StopCapture join 在有声场景下永不生效
        if (m_captureExit.load())
            break;
        {
            std::lock_guard lock(m_mutex);
            if (m_subscribers.empty())
            {
                m_captureExit.store(true);
                break;
            }
        }
    }

    client->Stop();

    // ── 收尾:锁内标记停止并释放资源(幂等) ──
    {
        std::lock_guard lock(m_mutex);
        m_capturing = false;
        m_captureExit.store(true);
        if (m_opusEncoder)
        {
            static_cast<IAudioClient*>(m_audioClient)->Release();
            static_cast<IAudioCaptureClient*>(m_captureClient)->Release();
            CloseHandle(static_cast<HANDLE>(m_captureEvent));
            opus_encoder_destroy(static_cast<OpusEncoder*>(m_opusEncoder));
            m_opusEncoder   = nullptr;
            m_audioClient   = nullptr;
            m_captureClient = nullptr;
            m_captureEvent  = nullptr;
        }
        // 设备失效:通知所有订阅者结束流;全程持 m_mutex,
        // 发送线程无法并发 erase,指针不会悬垂(锁内 notify 合法)
        for (auto& [task, sub] : m_subscribers)
        {
            std::lock_guard lk(sub->mtx);
            sub->stopped = true;
        }
        for (auto& [task, sub] : m_subscribers)
            sub->cv.notify_all();
    }
    DEFAULT_LOG_INFO("[audio] 采集停止(无订阅者/服务停止/设备失效)");
    CoUninitialize();
}

// ============================================================================
// 帧分发(内部加锁,分配全局递增 seq)
// ============================================================================

void ServerAudioStreamModule::DispatchFrame(const unsigned char* data, int len)
{
    std::lock_guard lock(m_mutex);
    if (m_subscribers.empty())
        return;   // 无订阅者:放弃帧(采集线程将在 200ms 内自行停止)

    uint32_t seq = m_nextSeq++;
    for (auto& [task, sub] : m_subscribers)
    {
        bool pushed = false;
        {
            std::lock_guard lk(sub->mtx);
            if (sub->stopped)
                continue;
            if (sub->queue.size() >= kAudioSubQueueMax)
                sub->queue.pop_front();   // 网络慢:丢最旧帧,实时性优先
            sub->queue.emplace_back(seq, std::vector<uint8_t>(data, data + len));
            pushed = true;
        }
        if (pushed)
            sub->cv.notify_one();
    }
}
// ============================================================================
// 订阅者发送线程:队列取帧 → SendReplyChunk;退出时自我清理
// ============================================================================

void ServerAudioStreamModule::SenderThreadMain(ServerAudioStreamModule* mgr, Subscriber* sub)
{
    BYTE hdr[8];
    while (true)
    {
        std::pair<uint32_t, std::vector<uint8_t>> frame;
        bool gotFrame = false;
        {
            std::unique_lock lk(sub->mtx);
            sub->cv.wait_for(lk, std::chrono::milliseconds(100), [&] {
                return sub->stopped || !sub->queue.empty();
            });
            if (sub->stopped)
                break;
            if (sub->queue.empty())
            {
                // 100ms 超时:流已结束或连接已断开 → 退出
                // (静音期间无帧可查,必须在此检查,否则客户端断开永远无法发现)
                if (!sub->task->IsStreaming() || sub->task->IsConnClosed())
                    break;
                continue;
            }
            frame = std::move(sub->queue.front());
            sub->queue.pop_front();
            gotFrame = true;
        }
        if (!gotFrame)
            continue;

        // 连接已断开/流已结束 → 退出
        if (!sub->task->IsStreaming() || sub->task->IsConnClosed())
            break;

        WriteLE32(hdr, (uint32_t)frame.second.size());
        WriteLE32(hdr + 4, frame.first);
        if (sub->task->IsConnClosed())
            break;   // 发送前再查一次,封死发送窗口
        sub->task->SendReplyChunk(hdr, 8);
        if (sub->task->IsConnClosed())
            break;   // 两组 chunk 之间也查一次
        sub->task->SendReplyChunk(frame.second.data(), frame.second.size());
    }

    // ── 自我清理 ──
    bool tasksGone = false;
    {
        std::lock_guard lock(mgr->m_mutex);
        mgr->m_subscribers.erase(sub->task);
        // 最后一个订阅者退出 → 发停采信号(采集线程自行收尾并回 IDLE)。
        // 注意:不 join、不置 m_capturing——线程回收由下次 Subscribe
        // (join 旧线程)或析构 StopCapture 统一完成,避免并发 join 同一
        // thread 对象导致 UB
        if (mgr->m_capturing && mgr->m_subscribers.empty())
        {
            mgr->m_captureExit.store(true);
            if (mgr->m_captureEvent)
                SetEvent(static_cast<HANDLE>(mgr->m_captureEvent));   // 立即唤醒采集线程
        }
        // 移交所有权给管理器(zombie)后退出——发送线程绝不 delete 自身:
        // 其 std::thread 成员仍 joinable,析构会 std::terminate(实测崩溃)
        mgr->m_zombies.push_back(sub);
        tasksGone = mgr->m_tasksGone;   // 锁内读取,与析构写侧同步(契约:仅锁内访问)
    }
    // 服务停止时(NetDock 已先于 ServicePortal 销毁)task/loop 已失效,
    // 跳过流收尾;运行期(连接断开/设备失效)则正常收尾
    if (!tasksGone)
    {
        if (sub->loop)
            sub->loop->TryReply();   // ★ 先取回复门:与 CLOSE 的 ProcessClose 驱动互斥,防双事件双回收
        sub->task->EndStreamReply();   // 线程安全:驱动 doer 回收(STREAM_END)
        // 原 tap->Drop():A 实例回池。经 PostToLoop 投递,由 A 线程 ProcessDone
        // 执行 TryReply + Release(投递时 epoch 校验,陈旧投递安全丢弃)
        if (sub->loop)
            sub->loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, sub->task);
        // ★ ctx 必须为 task(ProcessDone 身份校验契约,Task 2 审查定)
    }
    // 不 delete:Subscriber 由管理器 ReapZombies() join 后统一回收
    return;
}

// ============================================================================
// 停止采集(析构路径):join 采集线程,资源由采集线程收尾时释放
// ============================================================================

void ServerAudioStreamModule::StopCapture()
{
    {
        std::lock_guard lock(m_mutex);
        m_captureExit.store(true);
        if (m_captureEvent)
            SetEvent(static_cast<HANDLE>(m_captureEvent));   // 唤醒等待中的采集线程
    }
    if (m_captureThread.joinable())
        m_captureThread.join();
}
