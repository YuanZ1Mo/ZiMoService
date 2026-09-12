#pragma once
// ============================================================================
// NetDock:网络层宿主
//  持有三个服务器管理器(前端 80/443 / JRPC 39440 / RESTful 39441),
//  负责全局运行参数(经 ZmHttpServer::Init 一次性注入)与三面监听/根路径配置;
//  生命周期(Init/Open/Close)为进程级静态一次,由 ZmHttpServer 承载,本类只做转发。
//  对外暴露三个服务器引用,由业务层(ServicePortal)注册全部路由。
//  设计:docs/designs/2026-08-30-drogon-httpserver-base-design.md
// ============================================================================

#include <memory>
#include <string>

#include "http_frontend_manager.h"
#include "http_jsonrpc_manager.h"
#include "http_restful_manager.h"

#include "zm_net_http_server.h"

/**
 * @brief 取当前进程可执行文件所在目录
 *
 * 资源路径遵循 "exe 同级" 约定:静态资源在 <exeDir>frontend,证书在 <exeDir>certs。
 *
 * @return 以路径分隔符结尾的目录串(如 "A:\ZiMo\svc\");取路径失败时返回空串
 */
std::string ZmExeDir();

class NetDock
{
public:
    NetDock() = default;
    ~NetDock() = default;

    /**
     * @brief 网络层一次性初始化(Phase1,须先于 Open)
     *
     * 依次完成:全局参数/证书注入(ZmHttpServer::Init)→ CORS 白名单声明 →
     * 三面监听与根路径配置 → 三面结构路由/门禁 advice 注册。
     * 三面的对象都只创建与登记,不启动服务。
     *
     * @return true 全部成功;false 全局 Init 失败(此后不应再 Open)
     */
    bool Init();

    /// @brief 启动三面服务器(Phase2;后台线程跑事件循环)
    /// @return 绑定失败(端口占用等)或未 Init 返回 false
    bool Open() { return ZmHttpServer::Open(); }

    /// @brief 关闭三面服务器并回收线程(Phase3;幂等,关闭后为终态)
    void Close() { ZmHttpServer::Close(); }

    // ── 业务层路由注册入口(Phase1 内调用) ──
    /// @return 前端面实例(80/443;业务层注册页面路由与门禁 advice 用)
    ZmHttpFrontendServer* GetFrontendServer() { return m_frontend->GetServer(); }
    /// @return JSON-RPC 面实例(39440)
    ZmHttpJsonRpcServer* GetJsonRpcServer() { return m_jrpc->GetServer(); }
    /// @return RESTful 面实例(39441;业务 API 主入口)
    ZmHttpRestfulServer* GetRestfulServer() { return m_restful->GetServer(); }

    /**
     * @brief 热重载全局证书(运行期唯一可热更新能力)
     *
     * 证书为进程级全局,热重载只换内容不换路径。
     *
     * @return true 重载成功;false 无证书配置或重载失败
     */
    bool ReloadCertificates();

private:
    std::unique_ptr<HttpFrontendManager> m_frontend;
    std::unique_ptr<HttpJsonRpcManager> m_jrpc;
    std::unique_ptr<HttpRestfulManager> m_restful;
};
