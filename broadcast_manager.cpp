#include "broadcast_manager.h"
#include "zm_logger.h"

BroadcastManager::BroadcastManager()
    : m_evLoop(nullptr)
    , m_server(nullptr)
    , m_port(0)
{
}

BroadcastManager::~BroadcastManager()
{
    Close();
}

bool BroadcastManager::Open(uint16_t port)
{
    if (m_server)
        return true;

    // 创建内部事件循环线程
    m_evLoop = new ZmEvBaseRunLoop("BroadcastServerLoop");
    if (!m_evLoop->Loop())
    {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Failed to start event loop");
        delete m_evLoop;
        m_evLoop = nullptr;
        return false;
    }

    m_config.listenIp = "0.0.0.0";
    m_config.listenPort = port;
    m_config.evbase = m_evLoop->GetEventBase();
    m_config.heartbeatTime = 60;
    m_config.handshakeTimeout = 10;
    m_config.clientQueueMaxSize = 1024;

    m_callbacks.onListenSuccess = [this](uint16_t actualPort) {
        m_port = actualPort;
        DEFAULT_LOG_INFO("[BroadcastMgr] Listening on port {}", actualPort);
    };

    m_callbacks.onListenFailed = [](const std::string& error) {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Listen failed: {}", error);
    };

    m_callbacks.onListenStopped = []() {
        DEFAULT_LOG_INFO("[BroadcastMgr] Server stopped");
    };

    m_callbacks.onError = [](const std::string& error) {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Error: {}", error);
    };

    m_callbacks.onClientOnline = [](const BcClientInfo& info) {
        DEFAULT_LOG_INFO("[BroadcastMgr] Client online: {} ({}:{})",
                         info.clientId, info.ip, info.port);
    };

    m_callbacks.onClientOffline = [](const BcClientInfo& info) {
        DEFAULT_LOG_INFO("[BroadcastMgr] Client offline: {} ({}:{})",
                         info.clientId, info.ip, info.port);
    };

    m_server = new ZmBroadcastServer(m_config, m_callbacks);

    if (!m_server->Start())
    {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Start failed");
        delete m_server;
        m_server = nullptr;
        return false;
    }

    return true;
}

void BroadcastManager::Close()
{
    // ★ 先停 server（释放监听器和客户端连接，依赖 event_base 存活）
    if (m_server)
    {
        m_server->Stop();
        delete m_server;
        m_server = nullptr;
    }
    // 再停事件循环（server 已释放所有事件，event_base 可安全停止）
    if (m_evLoop)
    {
        m_evLoop->Stop();
        delete m_evLoop;
        m_evLoop = nullptr;
    }
    m_port = 0;
}

bool BroadcastManager::IsOpen() const
{
    if (!m_server)
        return false;
    ZM_BROADCAST_STATE state = m_server->GetState();
    return state == ZM_BC_STATE_LISTENING;
}

uint16_t BroadcastManager::GetPort() const
{
    return m_port;
}

int BroadcastManager::GetConnectionCount() const
{
    if (m_server)
        return m_server->GetConnectionCount();
    return 0;
}

uint64_t BroadcastManager::GetSentCount() const
{
    if (m_server)
        return m_server->GetSentCount();
    return 0;
}

bool BroadcastManager::Broadcast(const std::string& topic, const std::string& content,
                                  const std::string& tag)
{
    if (!m_server)
        return false;
    return m_server->Broadcast(topic, content, tag);
}

bool BroadcastManager::Send(const std::string& clientId, const std::string& topic,
                             const std::string& content, const std::string& tag)
{
    if (!m_server)
        return false;
    return m_server->Send(clientId, topic, content, tag);
}
