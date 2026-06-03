//// 控制模块:目前控制会话和rpc消息
//
//#ifndef _CONTROL_MODEL_H_
//#define _CONTROL_MODEL_H_
//
////#include "../Public/y_public_include.h"
//
//#include "message_callback.h"
//#include "../Public/websocket/y_websocket_server.h"
//#include "zm_net_http.h"
//#include "zm_net_socket.h"
//
//bool Trust_JRPCViaNetwork(const char* reqjs, std::string& rspjs);
//int  Trust_JSONRpc(const char* reqjs, std::string& repjs);
//
//
//class YServiceMessageModule;
//class YCommunicateCtrlModule;
//
//extern std::unique_ptr<YServiceMessageModule> g_y_service_message_module;
//extern std::unique_ptr<YCommunicateCtrlModule> g_y_communicate_ctrl_module;
//#ifdef __cplusplus
//extern "C"
//{
//#endif
//    extern void(__stdcall* g_y_service_async_callback)(const char* topic, const char* contentjs, bool bNotifyUpMessage);
//#ifdef __cplusplus
//}
//#endif
//
//
////会话持久化模块
////TODO  心跳
//class YSessionModule
//{
//public:
//    YSessionModule() {}
//    ~YSessionModule() {}
//
//
//public:
//    inline int  State()        const { return _state; }
//    void ChangeState(int state);
//
//    inline bool IsOnline()     const { return _online; }
//
//    void Clear();
//
//private:
//    int                         _state;
//    bool                        _online;
//};
//
////服务内部消息通讯
//class YServiceMessageModule
//{
//public:
//    YServiceMessageModule()
//    {
//        InitializeCB(&ServiceAsyncCallback);
//    };
//    ~YServiceMessageModule() {}
//
//    void InitializeCB(void(__stdcall* callback)(const char* topic, const char* contentjs, bool bNotifyUpMessage) = nullptr);
//    
//    /** 通知应用层,本函数为异步线程通知,实际就是开线程做了一次回调然后再通过YMessageServer发送rpc消息 */
//    void NotifyMessage(const char* topic, const ZMJSON& content = ZMJSON(), bool bNotifyUpMessage = false, int delay_mills = 0);
//
//
//    inline       YSessionModule* Session() { return &_session; }
//
//private:
//    YSessionModule              _session;
//};
//
//
//
//
//
//
//
//
//
//
//
//class YCommunicateCtrlModule
//{
//public:
//    YCommunicateCtrlModule();
//    ~YCommunicateCtrlModule() {}
//
//    void Restart();
//    void Start();
//    void Stop();
//
//
//    /* contentjs必须是json的格式的字符串,比如:{"testnode":"testmsg"} */
//    void PushNotify(const char* topic, const char* contentjs);
//
//private:
//    ZmJsonRpcServer             _http_server;
//    //YWinPipeServer             _pipe_server;
//    YMessageServer             _MessageServer;
//
//    //YThread_shared_ptr<MyThread> aaa;
//
//    enum
//    {
//        TERM_TYPE_NAMEDPIPE = 0,
//        TERM_TYPE_HTTP = 1,
//        TERM_TYPE_WEBSOCKET = 2
//    };
//
//    // Named Pipe 
//    void OnPipeRequest(const BYTE* qdata, size_t qdlen, ZmByteBuffer& repbuf);
//
//    // HTTPD JSONRpc
//    int  OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);
//    int  OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method, const ZMJSON& params, ZMJSON& result, ZMJSON& error);
//
//    void OnTermRequest(const ZMJSON& request, ZMJSON& reply, int termType = TERM_TYPE_NAMEDPIPE);
//};
//
//
//
//
//
//
//
//
//
//
//
//#endif /* _CONTROL_MODEL_H_ */
