#define _CRT_SECURE_NO_WARNINGS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <tchar.h>

#include "zm_logger.h"
#include "service_center.h"
#include "zm_util_str.h"



int _tmain(int argc, TCHAR* argv[])
{
    //while (true)
    //{
    //    Sleep(1000);
    //}

    ServiceCenter service;

    if (argc>1)
    {
        if (strcasecmp(argv[1], _T("install")) == 0)
        {
            DEFAULT_LOG_INFO("Install Begin");
            ZMServiceManager::Install(service);
            DEFAULT_LOG_INFO("Install End");
        }
        else if (strcasecmp(argv[1], _T("uninstall")) == 0)
        {
            DEFAULT_LOG_INFO("Uninstall Begin");
            ZMServiceManager::Uninstall(service);
            DEFAULT_LOG_INFO("Uninstall End");
        }
        else if (strcasecmp(argv[1], _T("debug")) == 0)
        {
            service.RunDebugMode(argc, argv);
        }
    }
    else
    {
        DEFAULT_LOG_INFO("ZiMo服务启动");
        if (service.Run())
        {
            DEFAULT_LOG_INFO("ZiMo服务正常停止");
        }
        else
        {
            DEFAULT_LOG_INFO("ZiMo服务异常停止");
        }
    }

    return 0;
}