//// 本地 WebSocket 广播服务器，负责向所有已连接的客户端推送消息
//
//#ifndef MESSAGE_SERVER_H_
//#define MESSAGE_SERVER_H_
//
//#include "zm_websocket_server.h"
//
//class MessageServer
//{
//public:
//    MessageServer();
//    ~MessageServer();
//
//    // 启动服务器，初始化 Winsock 并开始监听端口
//    void Start();
//
//    // 停止服务器，关闭所有客户端连接
//    void Stop();
//
//    // 向所有客户端推送通知
//    // topic: 消息主题，客户端可据此过滤感兴趣的消息
//    // contentjs: 必须是 JSON 格式的字符串，例如 {"testnode":"testmsg"}
//    void PushNotify(const char* topic, const char* contentjs);
//
//private:
//    ZmMessageServer*  m_message_server;  // WebSocket 服务器实例
//};
//
//#endif // MESSAGE_SERVER_H_