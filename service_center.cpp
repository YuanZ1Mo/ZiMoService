#include "service_center.h"

#include "net_dock.h"
#include "service_global.h"
#include "service_portal.h"
#include "service_define.h"

#include "zm_logger.h"

#include <Wtsapi32.h>
#pragma comment(lib, "Wtsapi32.lib")

// ============================================================================
// 生命周期回调
// ============================================================================

void ServiceCenter::OnStart(DWORD /*argc*/, TCHAR** /*argv[]*/)
{
    //initMessageServer();
    //initHttpServer();

    m_servicePortal = new ServicePortal();

    m_netDock = new NetDock();
    m_netDock->Init();

    // 将 NetDock 注入 ServicePortal，供状态查询等用途
    // 业务层在 ZmReqLoop 线程处理完成后，通过 ZmReqLoopJrpc::ResponseJson / ZmReqLoopRest 回复 helper 安全回写响应
    m_servicePortal->SetNetDock(m_netDock);

    m_netDock->SetJrpcRequestReadCB(std::bind(&ServicePortal::JrpcRequestReadCB, m_servicePortal,
        std::placeholders::_1, std::placeholders::_2));
    m_netDock->SetRESTfulRequestCB(std::bind(&ServicePortal::RestfulRequestCB, m_servicePortal,
        std::placeholders::_1, std::placeholders::_2,
        std::placeholders::_3));

    m_netDock->OpenHttpJsonRpcServer();
    m_netDock->OpenHttpRESTfulServer();
    m_netDock->OpenHttpServer();
    m_netDock->OpenBroadcastServer();

    // 注册 HTTP 80 端口路由（/control → 控制中心 SPA）
    m_servicePortal->RegisterHttpRoutes(m_netDock->GetHttpServerManager());
}

void ServiceCenter::OnStop()
{
    // ★ 关闭顺序:
    //   ① 先清除 ServicePortal 的 NetDock 引用 — 防止 NetDock 析构后
    //      ServicePortal 通过悬空指针访问已释放的 NetDock
    //   ② NetDock 释放 — 内部先停各 HTTP 前端(停服务器+排空 worker+停 ZmReqLoopPool),
    //      排空在飞请求后业务回调不再被调用
    //   ③ ServicePortal 释放 — 此时无新回调进入;远程音频发送线程通过
    //      m_tasksGone 标记跳过已失效的 task/loop 访问
    if (m_servicePortal)
        m_servicePortal->SetNetDock(nullptr);

    // 业务层停止钩子(内部处理音频模块 task/loop 失效标记):须在 NetDock 析构前调用。
    // NetDock 析构期间连接关闭触发 closecb,发送线程可能提前退出——标志未置位时
    // EndStreamReply/PostToLoop 会访问正在销毁的 task/loop(实测 0xc0000005)
    if (m_servicePortal)
        m_servicePortal->Shutdown();

    if (m_netDock)
    {
        delete m_netDock;
        m_netDock = nullptr;
    }

    if (m_servicePortal)
    {
        delete m_servicePortal;
        m_servicePortal = nullptr;
    }
}

void ServiceCenter::OnShutdown(DWORD /*evtType*/, WTSSESSION_NOTIFICATION* /*notification*/)
{
    DEFAULT_LOG_INFO("系统关机");
}

// ============================================================================
// 会话变更：监控用户登录/登出/锁屏/远程连接等
// ============================================================================

void ServiceCenter::OnSessionChange(DWORD evtType, WTSSESSION_NOTIFICATION* notification)
{
    // 查询触发会话的用户名
    TCHAR* buf = nullptr;
    DWORD size = 0;
    BOOL res = ::WTSQuerySessionInformation(
        nullptr, notification->dwSessionId, WTSUserName, &buf, &size);

    String message;
    if (res)
        message = String(buf) + _T(" ");
    else
        message = _T("未知用户 ");

    ::WTSFreeMemory(buf);

    // 映射会话事件类型
    switch (evtType)
    {
    case WTS_CONSOLE_CONNECT:    message += _T("已连接");           break;
    case WTS_CONSOLE_DISCONNECT: message += _T("断开连接");        break;
    case WTS_REMOTE_CONNECT:     message += _T("被远程连接");      break;
    case WTS_REMOTE_DISCONNECT:  message += _T("被断开远程连接");  break;
    case WTS_SESSION_LOGON:      message += _T("登录");            break;
    case WTS_SESSION_LOGOFF:     message += _T("登出");            break;
    case WTS_SESSION_LOCK:       message += _T("锁定计算机");      break;
    case WTS_SESSION_UNLOCK:     message += _T("解锁计算机");      break;
    default:                     message += _T("执行了未跟踪的操作"); break;
    }

    DEFAULT_LOG_INFO(message.c_str());
}

// ============================================================================
// 电源事件：睡眠/恢复/AC电源切换/电源设置变更
// ============================================================================

// 电源设置 GUID（需与基类 registerPowerNotifications 中注册的保持一致）
static const GUID GUID_BATTERY_PCT =
    { 0xa7ad8041, 0xb45a, 0x4cae, { 0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1 } };
static const GUID GUID_MONITOR_BRIGHTNESS =
    { 0xaded5e82, 0xb909, 0x4619, { 0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb } };
static const GUID GUID_SLEEP_TIMEOUT =
    { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } };
static const GUID GUID_MONITOR_POWER =
    { 0x02731015, 0x4510, 0x4526, { 0xad, 0x22, 0xe8, 0x6d, 0x95, 0x1b, 0x08, 0x56 } };
static const GUID GUID_POWER_SAVING =
    { 0xe00958c0, 0xc213, 0x4dfc, { 0xa2, 0x0e, 0x40, 0x13, 0x74, 0x50, 0x4b, 0x5c } };

// 读取 POWERBROADCAST_SETTING 中的 DWORD 值并格式化
static std::string FormatPowerDword(const char* name, const BYTE* data, DWORD dataLen)
{
    if (dataLen >= 4)
        return fmt::format("{}: {}", name, *reinterpret_cast<const DWORD*>(data));
    return fmt::format("{}: 数据长度不足({}字节)", name, dataLen);
}

void ServiceCenter::OnPowerChange(DWORD evtType, POWERBROADCAST_SETTING* notification)
{
    std::string message;

    switch (evtType)
    {
    // AC/电池切换、电池电量变化
    case PBT_APMPOWERSTATUSCHANGE:
    {
        SYSTEM_POWER_STATUS ps = {};
        if (::GetSystemPowerStatus(&ps))
        {
            message = fmt::format("电源状态变化 ACLine={}, Battery={}%, ChargeStatus={}",
                ps.ACLineStatus, ps.BatteryLifePercent, ps.BatteryFlag);
        }
        else
        {
            message = "电源状态变化（获取详细信息失败）";
        }
        break;
    }

    // 系统即将进入睡眠（需快速返回，时间有限）
    case PBT_APMSUSPEND:
        message = "系统即将进入睡眠";
        break;

    // 系统自动唤醒（定时器等非用户操作触发）
    case PBT_APMRESUMEAUTOMATIC:
        message = "系统从睡眠自动恢复";
        break;

    // 用户操作触发唤醒（键鼠等），在 APMRESUMEAUTOMATIC 之后发送
    case PBT_APMRESUMESUSPEND:
        message = "系统从睡眠恢复（用户触发）";
        break;

    // 电量耗尽导致的紧急休眠后恢复
    case PBT_APMRESUMECRITICAL:
        message = "系统从紧急休眠恢复（电量耗尽）";
        break;

    // 电源设置变更（需在基类中 RegisterPowerSettingNotification 注册后才能收到）
    case PBT_POWERSETTINGCHANGE:
    {
        if (!notification)
        {
            message = "电源设置变更通知（数据为空）";
            break;
        }

        const GUID& guid = notification->PowerSetting;
        const BYTE* data = notification->Data;
        DWORD dataLen = notification->DataLength;

        if (guid == GUID_BATTERY_PCT)
            message = FormatPowerDword("电池剩余百分比", data, dataLen);
        else if (guid == GUID_MONITOR_BRIGHTNESS)
            message = FormatPowerDword("显示器亮度", data, dataLen);
        else if (guid == GUID_SLEEP_TIMEOUT)
            message = FormatPowerDword("系统睡眠超时", data, dataLen);
        else if (guid == GUID_MONITOR_POWER)
            message = FormatPowerDword("显示器电源状态", data, dataLen);
        else if (guid == GUID_POWER_SAVING)
            message = FormatPowerDword("节能模式状态", data, dataLen);
        else
            message = fmt::format("未知电源设置变更 数据长度={}", dataLen);

        break;
    }

    default:
        message = fmt::format("未处理的电源事件: {}", evtType);
        break;
    }

    DEFAULT_LOG_INFO(message);
}