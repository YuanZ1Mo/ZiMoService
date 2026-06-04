#ifndef NET_DOCK_H
#define NET_DOCK_H

#include "zm_websocket_server.h"
#include "zm_net_http.h"


#include "service_global.h"

#include "dock_runloop.h"

#include "zm_net_tap_hub.h"
#include "zm_net_tap_dnr.h"
#include "zm_net_tap_jrpc.h"

class NetDock
{
public:
    NetDock();
    ~NetDock();

    void Init();

    void OpenWebSocketServer();
    void CloseWebSocketServer();

    void OpenHttpServer();
    void CloseHttpServer();

    void SetJrpcRequsetReadCB(TapDelegateJrpcRequsetReadCB cb)
    {
        _tapDelegateJrpcRequsetReadCB = cb;
    }


    /** 调度到_dock_runloop创建的LibEvent主线程中执行 */
    bool     ScheduleTaskInLoop(std::function<void(void*)> fn_run, std::function<void(void*)> fn_cb, void* param);



private:
    static void OnSheculeEventCB(evutil_socket_t fd, short what, void* ctx);


    int OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method, const ZMJSON& params, ZMJSON& result, ZMJSON& error);
    bool SendToHubProxy(const char* reqjs, std::string& rspjs);

public:



private:
    event_base* _evbase;
    evdns_base* _evdnsbase;
    ZmTapContext*            _tapContext;

    DockRunLoop* _dock_runloop;

    ZmMessageServer* _messageServer;  // WebSocket 服务器实例
    ZmJsonRpcServer*           _httpServer_JRPC;


    ZmTapDelegateJRPC* _tapDelegate_JRPC;
    ZmTapDomainNameResolver* _tapDomainNameResolver;
    ZmTapHubProxy* _tapHubProxy;
    uint16_t _hubProxyPort;

    TapDelegateJrpcRequsetReadCB _tapDelegateJrpcRequsetReadCB;
};

#endif // NET_DOCK_H