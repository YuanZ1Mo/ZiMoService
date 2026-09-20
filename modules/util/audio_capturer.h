#ifndef ZM_UTIL_AUDIO_CAPTURER_H
#define ZM_UTIL_AUDIO_CAPTURER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

struct IAudioClient;
struct IAudioCaptureClient;
struct OpusEncoder;

/**
 * @brief 系统声音采集:默认播放设备的回环(WASAPI loopback)+ Opus 编码
 *
 * 把"本机正在播放的声音"变成连续的 20ms Opus 帧交给上层:
 *   - 采集对象是**默认渲染端点**的回环输出,不占用麦克风,也不改动本机音量;
 *   - 混音格式与目标格式(48kHz / 2ch / int16)不一致时先问引擎能否接受 48kHz 候选,
 *     都不行再请引擎插入采样率转换;三种位深(16bit、32bit、32bit 浮点)与
 *     单/多声道都能转;
 *   - 引擎标记为静音的包**照常编码**(产出静音帧):播放端的时间轴跟数据走,
 *     静音期间不产片会让播放停在缓冲末尾并永久累积落后量。
 *
 * 线程模型:
 *   - 设备枚举、格式协商、引擎启动、编码器创建都在 **Start() 的调用线程**上完成,
 *     失败即同步返回(调用方据此回 503),因此调用线程需能承受数百毫秒阻塞;
 *   - 启动成功后由内部采集线程泵包:每个循环周期回调 onFrame(有帧)与 onTick(必回调),
 *     线程退出前回调 onDone;
 *   - RequestStop() 可从任意线程调用(含采集线程自身);StopAndJoin() 会 join,
 *     不得在采集线程上调用。
 */
class ZmAudioCapturer
{
public:
    /// @brief 帧回调:20ms 一帧(Opus 包 + 采集时间轴上的毫秒位置)
    using FrameCb = std::function<void(const uint8_t* data, size_t len, uint64_t ptsMs)>;
    /// @brief 周期回调:每个采集循环周期调用一次(用于扫描听众、判断停采)
    using TickCb = std::function<void()>;
    /// @brief 退出回调:采集线程收尾后调用一次(设备已释放)
    using DoneCb = std::function<void()>;

    ZmAudioCapturer();
    ~ZmAudioCapturer();

    ZmAudioCapturer(const ZmAudioCapturer&) = delete;
    ZmAudioCapturer& operator=(const ZmAudioCapturer&) = delete;

    /**
     * @brief 启动采集(同步完成设备协商与引擎启动)
     *
     * @param onFrame 帧回调(在采集线程上调用,回调内不得阻塞)
     * @param onTick  周期回调(在采集线程上调用;每个循环周期一次,含超时分支)
     * @param onDone  退出回调(在采集线程上调用一次,此时设备与编码器已释放)
     * @param errMsg  [out] 失败原因(用于日志与接口 503 的说明)
     * @return true 启动成功;false 无默认播放设备/格式不支持/引擎启动失败
     * @note 启动前会先回收上一次已退出的采集线程,可重复调用
     */
    bool Start(FrameCb onFrame, TickCb onTick, DoneCb onDone, std::string& errMsg);

    /**
     * @brief 请求停止(线程安全,不 join)
     *
     * 置停止标志并唤醒等待中的采集线程;采集线程在 200ms 内退出并回调 onDone。
     * 采集线程自身也用它来请求停采(例如已无人收听)。
     */
    void RequestStop();

    /// @brief 请求停止并等待采集线程退出(幂等;不得在采集线程上调用)
    void StopAndJoin();

    /// @return 采集线程是否在运行
    bool IsRunning() const { return m_running.load(); }

    /**
     * @brief 查询 Opus 前置跳过量(采样数)
     *
     * 供 WebM 封装填写 CodecDelay 与 OpusHead 的 preSkip;20ms 帧 /
     * 48kHz 下由 libopus 决定,不写死。
     *
     * @return preSkip 采样数;编码器创建失败时为 0
     */
    static int GetPreSkip();

private:
    /// @brief 采集线程主体:泵包 → 转换 → 编码 → 回调
    void ThreadMain();

    FrameCb m_onFrame;
    TickCb  m_onTick;
    DoneCb  m_onDone;

    std::thread       m_thread;
    std::atomic<bool> m_exit{false};
    std::atomic<bool> m_running{false};

    // 采集资源(Start 创建;采集线程收尾时释放并置空)
    IAudioClient*        m_client = nullptr;
    IAudioCaptureClient* m_capture = nullptr;
    OpusEncoder*         m_encoder = nullptr;
    /// 就绪事件 HANDLE(自动重置):RequestStop 在别的线程读它,采集线程收尾时置空,
    /// 故用原子交换而不是裸指针(最坏情况只是对已关闭句柄 SetEvent 返回失败)
    std::atomic<void*> m_event{nullptr};
    int                  m_mixChannels = 0; ///< 混音格式快照
    int                  m_mixBits = 0;
    bool                 m_mixFloat = false;
};

#endif // ZM_UTIL_AUDIO_CAPTURER_H
