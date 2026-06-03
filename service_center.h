#ifndef SERVICE_CENTER_H
#define SERVICE_CENTER_H

#include "zm_service_base.h"
#include "name_define.h"
#include "net_dock.h"

// ZiMo 服务主控中心
// 管理消息服务、会话变更、电源事件、关机等生命周期
class ServiceCenter : public ZmServiceBase
{
public:
    ServiceCenter(const ServiceCenter&) = delete;
    ServiceCenter& operator=(const ServiceCenter&) = delete;
    ServiceCenter(ServiceCenter&&) = delete;
    ServiceCenter& operator=(ServiceCenter&&) = delete;

    ServiceCenter()
        : ZmServiceBase(
            SERVICE_NAME,
            SERVICE_DETAIL,
            SERVICE_DESC,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE
            | SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_SHUTDOWN)
    {
    }

private:
    // ---- 生命周期回调 ----

    void OnStart(DWORD argc, WCHAR* argv[]) override;
    void OnStop() override;
    void OnShutdown(DWORD evtType, WTSSESSION_NOTIFICATION* notification) override;
    void OnSessionChange(DWORD evtType, WTSSESSION_NOTIFICATION* notification) override;
    void OnPowerChange(DWORD evtType, POWERBROADCAST_SETTING* notification) override;

    // ---- 内部方法 ----

    //void initMessageServer();
    //void unInitMessageServer();

    //void initHttpServer();
    //void unInitHttpServer();

private:
    NetDock* m_netDock;
};

#endif // SERVICE_CENTER_H_
