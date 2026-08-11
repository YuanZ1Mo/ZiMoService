#ifndef MODULE_SERVER_AUDIO_STREAM_H
#define MODULE_SERVER_AUDIO_STREAM_H

#include "zm_net_http.h"   // ZmHttpdTask / evhttp_cmd_type / BYTE

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class ZmReqLoop;   // 本请求的 A 实例(收尾投递用,仅存指针)
class UserModule;  // 注入:鉴权(AuthAndTouch)/模块权限(GetUserModules)

// ── 帧协议常量(小端) ─────────────────────────────────────
// 每帧 = len(4B) + seq(4B) + Opus 帧数据
constexpr uint32_t kAudioSampleRate   = 48000;   // 采样率 Hz
constexpr uint16_t kAudioChannels     = 2;       // 声道数(立体声)
constexpr uint32_t kAudioBitrate      = 64000;  // 编码码率 bps(立体声)
constexpr uint32_t kAudioFrameSamples = kAudioSampleRate / 50;  // 960 采样/帧(20ms)
constexpr uint32_t kAudioFrameMaxLen  = 1500;    // Opus 最大帧 1275B + 余量
constexpr uint32_t kAudioSubQueueMax  = 600;     // 每订阅者队列上限(600 帧 ≈ 12s)

/**
 * @brief 服务器音频流模块(业务层)
 *
 * 按需采集服务器系统声音(WASAPI loopback)→ Opus 编码 →
 * 向订阅的 HTTP 流式连接推送二进制帧。无订阅者时停止采集、释放音频设备。
 *
 * 线程模型:
 *   - 采集线程:WASAPI loopback 读 PCM → Opus 编码 → 分发帧到各订阅者队列;
 *     发现无订阅者时自行停止并释放资源(200ms 内)
 *   - 每个订阅者一个发送线程:队列取帧 → task->SendReplyChunk;
 *     连接断开或服务停止时退出并自我清理
 *   - 订阅表与状态机由 m_mutex 保护;队列由 Subscriber::mtx 保护
 */
class ServerAudioStreamModule
{
public:
    explicit ServerAudioStreamModule(UserModule* userModule);
    ~ServerAudioStreamModule();

    ServerAudioStreamModule(const ServerAudioStreamModule&) = delete;
    ServerAudioStreamModule& operator=(const ServerAudioStreamModule&) = delete;

    /**
     * @brief 订阅一个 HTTP 流式连接(必要时启动采集管线)
     * @param task 目标 HTTP 任务(调用方已 StartStreamReply)
     * @param loop 本请求的 A 实例(流收尾时经 PostToLoop(REQ_LOOP_SIG_DONE, task) 回池)
     * @return true 订阅成功;false 服务器无可用音频设备(采集启动失败)
     * @note 必须在 RESTful 业务回调(ZmReqLoop 线程)中调用
     */
    bool Subscribe(ZmHttpdTask* task, ZmReqLoop* loop);

    /**
     * @brief 标记 task/loop 即将失效(服务停止时须在 NetDock 析构前调用)
     * @note 断连检测(I1 closecb)使发送线程可能在 NetDock 析构期间提前退出:
     *       若 m_tasksGone 尚未置位,其 EndStreamReply/PostToLoop 会访问正在
     *       销毁的网络对象(实测 0xc0000005 崩溃)。幂等,析构中也会再次置位。
     */
    void SetTasksGone();

    /**
     * @brief 查询指定 task 是否仍是活跃订阅者(锁内检查订阅表)
     * @param task 目标 HTTP 任务
     * @return true 仍在订阅表中(发送线程正常运行);false 已被移除(如采集瞬时失败收尾)
     */
    bool IsSubscriberAlive(ZmHttpdTask* task) const;

    /**
     * @brief REST 分发入口(service_portal.cpp 的 else 链调用;位置在 user 之后、portal 之前)
     * @note 仅处理 /portal/serverAudioStream/*:stream(鉴权后流式订阅)/ status(状态查询)
     * @return true 本模块已处理(含错误响应);false 未命中,走 portal 原逻辑
     */
    bool DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                      ZmHttpdTask* task, const BYTE* body, size_t bodyLen);

    /** 音频状态快照(供 /portal/serverAudioStream/status) */
    struct AudioStatus
    {
        bool     capturing = false;
        int      subscriberCount = 0;
        uint32_t sampleRate = kAudioSampleRate;
        uint16_t channels = kAudioChannels;
        uint32_t bitrate = kAudioBitrate;
    };

    /** @brief 查询状态(锁内读采集标志与订阅数) */
    bool GetStatus(AudioStatus& out) const;

private:
    void HandleAudioStream(ZmReqLoop* loop, ZmHttpdTask* task);
    void HandleAudioStatus(ZmReqLoop* loop, ZmHttpdTask* task);

private:
    /** @brief 订阅者:每连接一个,持有发送线程与帧队列 */
    struct Subscriber
    {
        ZmHttpdTask* task = nullptr;
        ZmReqLoop*   loop = nullptr;   // 本请求的 A 实例(流收尾 PostToLoop 投递用)
        std::deque<std::pair<uint32_t, std::vector<uint8_t>>> queue;  // <seq, opus帧>
        std::mutex mtx;
        std::condition_variable cv;
        std::thread thread;
        bool stopped = false;   // 服务停止信号,置位后发送线程尽快退出
    };

    /** @brief 尝试启动采集管线(要求 m_mutex 已持有);失败时自行清理 */
    bool TryStartCaptureLocked();
    /** @brief 停止采集线程(join 并等待其自行释放资源);幂等 */
    void StopCapture();
    /**
     * @brief 回收已退出发送线程的 Subscriber(join 后 delete)
     * @note 发送线程退出时把自身 Subscriber 移交 m_zombies(锁内),
     *       由管理器统一回收——发送线程绝不能 delete 自身:其 std::thread
     *       成员仍 joinable,析构会触发 std::terminate(实测崩溃 0xc0000409)
     */
    void ReapZombies();

    /** @brief 采集线程主体 */
    void CaptureThreadMain();
    /** @brief 订阅者发送线程主体 */
    static void SenderThreadMain(ServerAudioStreamModule* mgr, Subscriber* sub);
    /** @brief 分发一帧 Opus 数据到所有订阅者队列(内部加锁,分配全局 seq) */
    void DispatchFrame(const unsigned char* data, int len);

    mutable std::mutex m_mutex;
    bool m_capturing = false;                                   // 采集状态(仅锁内访问)
    bool m_tasksGone = false;                                   // 析构已开始:task/loop 即将失效(仅锁内访问)
    UserModule* m_userModule = nullptr;                         ///< 注入:鉴权/模块权限(不拥有)
    std::atomic<bool> m_captureExit {false};
    std::thread m_captureThread;
    std::unordered_map<ZmHttpdTask*, Subscriber*> m_subscribers;  // 活跃订阅者(仅锁内访问)
    std::vector<Subscriber*> m_zombies;                         // 已退出发送线程,待 ReapZombies 回收(仅锁内访问)
    uint32_t m_nextSeq = 1;                                     // 全局递增序号(进程内;重启归零,注释修正)

    // 采集资源(仅采集线程使用;StopCapture 后置空):
    void* m_opusEncoder   = nullptr;    // OpusEncoder*
    void* m_audioClient   = nullptr;    // IAudioClient*
    void* m_captureClient = nullptr;    // IAudioCaptureClient*
    void* m_captureEvent  = nullptr;    // HANDLE
    int   m_mixChannels   = 0;          // 混音格式快照
    int   m_mixBits       = 0;
    bool  m_mixFloat      = false;
};

#endif // MODULE_SERVER_AUDIO_STREAM_H
