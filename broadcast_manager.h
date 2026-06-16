#ifndef BROADCAST_MANAGER_H
#define BROADCAST_MANAGER_H

#include "zm_net_broadcast_server.h"
#include "zm_net_runloop.h"

/**
 * @brief 广播服务端管理器
 *
 * 包装 ZmBroadcastServer，遵循 HttpServerManager 模式。
 * 提供生命周期管理和对外的状态查询 / 消息推送接口。
 */
class BroadcastManager
{
public:
    BroadcastManager();
    ~BroadcastManager();

    /**
     * @brief 启动广播服务端（内部自行创建事件循环线程）
     * @param port    监听端口（0 = 随机）
     * @return true 成功，false 失败
     */
    bool Open(uint16_t port);

    /** @brief 停止广播服务端 */
    void Close();

    /** @brief 是否正在运行 */
    bool IsOpen() const;

    /** @brief 获取实际监听端口 */
    uint16_t GetPort() const;

    /** @brief 获取当前连接数 */
    int GetConnectionCount() const;

    /** @brief 获取累计发送数 */
    uint64_t GetSentCount() const;

    /**
     * @brief 向所有匹配 tag 的客户端广播消息
     * @param topic    主题
     * @param content  内容（JSON 字符串）
     * @param tag      过滤标签
     * @return true 成功投递
     */
    bool Broadcast(const std::string& topic, const std::string& content, const std::string& tag);

    /**
     * @brief 向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     主题
     * @param content   内容
     * @param tag       过滤标签
     * @return true 成功投递
     */
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);

private:
    ZmEvBaseRunLoop*   m_evLoop;        ///< 内部事件循环线程
    ZmBroadcastServer* m_server;        ///< 底层广播服务端
    BcServerConfig     m_config;        ///< 服务端配置
    BcServerCallbacks  m_callbacks;     ///< 事件回调
    uint16_t           m_port;          ///< 实际监听端口
};

#endif // BROADCAST_MANAGER_H
