#ifndef DOCK_RUNLOOP_H
#define DOCK_RUNLOOP_H

#include "service_global.h"
#include "util/zm_util_thread.h"

#include <event2/dns.h>
#include <event2/event.h>

class DockRunLoop : public ZmThread
{
public:
    enum { CONTROL_LOOP_EXIT =0x0100,};
    
    DockRunLoop();
    virtual ~DockRunLoop();

    bool Loop();

    bool IsLooped();

    void Control(short events);

    event_base* GetEventBase();
    evdns_base* GetEventDnsBase();

protected:
    static  void    OnEventCtrlCB(evutil_socket_t fd, short what, void* arg);
    static  void    OnTimerCB(evutil_socket_t fd, short what, void* ctx);

    virtual void    Run();
    virtual void    OnStopping();
    
private:
    void            freeEventObjects();


private:

    event_base*  _evbase;
    evdns_base* _evdnsbase;
    event*       _eventTimer;
    event*       _eventCtrl;

    std::mutex              _mutex_loop;
    std::condition_variable _cv_loop;
    bool _b_looped;
    bool _b_run_finished;
};


#endif /* DOCK_RUNLOOP_H */