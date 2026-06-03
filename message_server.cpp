//#include "message_server.h"
//
//#include "zm_net_socket.h"
//
//
//MessageServer::MessageServer()
//{
//
//}
//
//MessageServer::~MessageServer()
//{
//
//}
//
//void MessageServer::Start()
//{
//    if (nullptr == m_message_server)
//    {
//        m_message_server = new ZmMessageServer();
//        // 设置 WebSocket 广播服务监听端口为 37310
//        m_message_server->SetBindPort(37310);
//        // 启动 WebSocket 服务器，开始监听连接
//        m_message_server->Start();
//    }
//}
//
//void MessageServer::Stop()
//{
//    if (nullptr != m_message_server)
//    {
//        // 停止 WebSocket 服务器，关闭所有客户端连接
//        m_message_server->Stop();
//        delete m_message_server;
//        m_message_server = nullptr;
//    }
//}
//
//// 向所有已连接的客户端推送消息
//// topic: 消息主题，用于客户端过滤感兴趣的消息
//// contentjs: JSON 格式的消息内容，例如 {"testnode":"testmsg"}
//void MessageServer::PushNotify(const char* topic, const char* contentjs)
//{
//    // 通过 WebSocket 按主题广播消息给所有客户端
//    m_message_server->PostNotificationWithTopic(topic, contentjs);
//}