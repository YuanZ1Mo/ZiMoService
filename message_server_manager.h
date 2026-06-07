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

    /**
     * @brief 启动 WebSocket 服务器（端口 ZM_WS_SERVER_PORT），重复调用安全
     * @example
     *   MessageServerManager mgr;
     *   mgr.Open();   // 首次启动
     *   mgr.Open();   // 重复调用，安全跳过
     */
    void Open();

    /**
     * @brief 停止 WebSocket 服务器并释放资源
     * @example
     *   mgr.Close();  // 停止服务器，释放内存，可安全重复调用
     */
    void Close();

    /**
     * @brief 获取 ZmMessageServer 原始指针，供外部设置回调
     * @return ZmMessageServer 指针（未启动时返回 nullptr）
     */
    ZmMessageServer* Server() { return m_messageServer; }

private:
    ZmMessageServer* m_messageServer;  ///< WebSocket 服务器实例
};

#endif // MESSAGE_SERVER_MANAGER_H
