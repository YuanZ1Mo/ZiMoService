#ifndef MESSAGE_SERVER_MANAGER_H
#define MESSAGE_SERVER_MANAGER_H

#include "zm_websocket_server.h"

/**
 * @brief WebSocket 服务器管理器，负责 ZmMessageServer 的完整生命周期
 */
class MessageServerManager
{
public:
    MessageServerManager();
    ~MessageServerManager();

    /** @brief 启动 WebSocket 服务器（端口 37310），重复调用安全 */
    void Open();

    /** @brief 停止 WebSocket 服务器并释放资源 */
    void Close();

    /** @brief 获取 ZmMessageServer 原始指针（可能为 nullptr），供外部设置回调 */
    ZmMessageServer* Server() { return m_messageServer; }

private:
    ZmMessageServer* m_messageServer;  ///< WebSocket 服务器实例
};

#endif // MESSAGE_SERVER_MANAGER_H
