#include "dock_runloop.h"

#include "zm_logger.h"
//#include "zm_simple_define.h"
#include "zm_util_sys.h"
#include "zm_util_libevent.h"
#include "zm_net_dns.h"
#include "zm_util_str.h"


enum { CONTROL_LOOP_SUCCESS = 0x0200, };

DockRunLoop::DockRunLoop(): ZmThread("DockRunLoop")
{
    _evbase     = nullptr;
    _evdnsbase = nullptr;
    _eventCtrl  = nullptr;
    _eventTimer = nullptr;
    _b_looped = false;
    _b_run_finished = false;
}

DockRunLoop::~DockRunLoop()
{

}

void DockRunLoop::freeEventObjects()
{
    //SP_DEV_LOGT("[runloop] Free the event objects");
    if ( _eventCtrl )
    {
        event_free(_eventCtrl);
        _eventCtrl = nullptr;
    }

    if (_evdnsbase)
    {
        evdns_base_free(_evdnsbase, 0);
        _evdnsbase = nullptr;
    }

    if ( _eventTimer )
    {
        event_free(_eventTimer);
        _eventTimer = nullptr;
    }

    if ( _evbase )
    {
        event_base_free(_evbase);
        _evbase = nullptr;
    }

    _b_looped = false;
}

bool DockRunLoop::Loop()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    if (!_b_looped)
    {
        Start();
        _cv_loop.wait(lock, [this] { return _b_looped || _b_run_finished; });
    }

    return _b_looped;
}

bool DockRunLoop::IsLooped()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _b_looped;
}

void DockRunLoop::Control(short events)
{
    //SP_DEV_LOGT("[runloop] Performing control: cmd=%04X", cmd);

    std::unique_lock<std::mutex> lock(_mutex_loop);

    if (IsRunning())
    {
        if (_eventCtrl)
        {
            event_active(_eventCtrl, events, 0);
        }
    }
}

event_base* DockRunLoop::GetEventBase()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _evbase;
}

evdns_base* DockRunLoop::GetEventDnsBase()
{
    std::unique_lock<std::mutex> lock(_mutex_loop);
    return _evdnsbase;
}

void DockRunLoop::Run()
{
    //Y_LOGT("[runloop] The runloop is running");

    zm_util_eventbase_init();

    freeEventObjects();

    _evbase = event_base_new();
    if (nullptr != _evbase)
    {
        _eventCtrl = event_new(_evbase, -1, EV_PERSIST | EV_READ, DockRunLoop::OnEventCtrlCB, (void*)this);
        event_add(_eventCtrl, 0);
        event_active(_eventCtrl, CONTROL_LOOP_SUCCESS, 0);

        _evdnsbase = evdns_base_new(_evbase, 0);

        // 从系统获取 DNS 服务器并配置到 evdns_base，使 evdns_getaddrinfo 可正常工作
        {
            std::string dns_addrs = ZmNetDNS::GetDNSAddresses();
            if (!dns_addrs.empty())
            {
                char* addrs_buf = _strdup(dns_addrs.c_str());
                if (addrs_buf)
                {
                    char* cursor = addrs_buf;   // zm_strsep 会修改它
                    char* token = zm_strsep(&cursor, ",");
                    while (token)
                    {
                        while (*token == ' ') token++;
                        if (*token)
                        {
                            evdns_base_nameserver_ip_add(_evdnsbase, token);
                            DEFAULT_LOG_INFO("Add DNS nameserver to evdns_base: {}", token);
                        }
                        token = zm_strsep(&cursor, ",");
                    }
                    free(addrs_buf);
                }
            }
        }

        //EV_PERSIST保证定时器循环触发
        _eventTimer = event_new(_evbase, -1, EV_TIMEOUT | EV_PERSIST, DockRunLoop::OnTimerCB, nullptr);
        timeval timer_second = { 60, 0 };
        event_add(_eventTimer, &timer_second);

        bool unexpected = false;

        // #define EVLOOP_ONCE              0x01
        // #define EVLOOP_NONBLOCK          0x02
        // #define EVLOOP_NO_EXIT_ON_EMPTY  0x04
        int ret = event_base_loop(_evbase, EVLOOP_NO_EXIT_ON_EMPTY);
        unexpected = (0 == event_base_got_exit(_evbase));
        //Y_LOGI("[runloop] RunLoop is broken %s: event_base_loop=%d, event_base_got_exit=%d",
        //    unexpected ? "*unexpected*" : "",
        //    ret, event_base_got_exit(_evbase));
        freeEventObjects();
    }
    else
    {
        //Y_LOGE("[runloop] Open event base failed");
    }

    {
        std::lock_guard<std::mutex> lock(_mutex_loop);
        _b_run_finished = true;
    }
    _cv_loop.notify_one();
    //Y_LOGI("[runloop] The runloop is stoped");
}

void DockRunLoop::OnStopping()
{
    Control(CONTROL_LOOP_EXIT);
}

void DockRunLoop::OnEventCtrlCB(evutil_socket_t fd, short what, void* arg)
{
    //SP_DEV_LOGT("[runloop] Received control event: fd=%d what=%04x arg=%p", (int)fd, what, arg);
    DockRunLoop* dockRunloop = (DockRunLoop*)arg;

    //剥离 libevent标准事件标志、保留自定义控制命令
    what = what & 0x7F00;
    if ((what & CONTROL_LOOP_EXIT) == CONTROL_LOOP_EXIT)
    {
        if ( nullptr!= dockRunloop->_evbase )
        {
            //SP_DEV_LOGT("%s event_base_loopexit", __FUNCTION__);

            /** event_base_loopbreak() 立即退出， event_base_loopexit() 完成未完成的任务后再退出 */
            //event_base_loopbreak(runloop->_evbase);
            event_base_loopexit(dockRunloop->_evbase, NULL);
        }
    }
    if ((what & CONTROL_LOOP_SUCCESS) == CONTROL_LOOP_SUCCESS)
    {
        {
            std::lock_guard<std::mutex> lock(dockRunloop->_mutex_loop);
            dockRunloop->_b_looped = true;
        }
        dockRunloop->_cv_loop.notify_one();
    }

}

void DockRunLoop::OnTimerCB(evutil_socket_t fd, short what, void* arg)
{
    char buf[32];
    ZmSystem::CurrentTimeStr(buf, sizeof(buf));
    DEFAULT_LOG_INFO("DockRunloop:{} HeartbeatTime:{}", arg, buf);
}