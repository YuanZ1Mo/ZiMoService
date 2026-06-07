#include "message_server_manager.h"

MessageServerManager::MessageServerManager()
    : m_messageServer(nullptr)
{
}

MessageServerManager::~MessageServerManager()
{
    Close();
}

void MessageServerManager::Open()
{
    if (nullptr == m_messageServer)
    {
        m_messageServer = new ZmMessageServer();
        // 设置 WebSocket 广播服务监听端口为 37310
        m_messageServer->SetBindPort(37310);
        // 启动 WebSocket 服务器，开始监听连接
        m_messageServer->Start();
    }
}

void MessageServerManager::Close()
{
    if (nullptr != m_messageServer)
    {
        // 停止 WebSocket 服务器，关闭所有客户端连接
        m_messageServer->Stop();
        delete m_messageServer;
        m_messageServer = nullptr;
    }
}
