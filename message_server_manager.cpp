#include "message_server_manager.h"
#include "service_define.h"

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
        m_messageServer->SetBindPort(ZM_WS_SERVER_PORT);
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
