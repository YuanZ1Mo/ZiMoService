#pragma once
// ============================================================================
// NetDock:网络层宿主(重建,Drogon 版)
//  持有三个服务器管理器(前端 80/443 / JRPC 39440 / RESTful 39441),
//  负责全局运行参数(经 ZmHttpServer::Init 一次性注入)与三面配置/路由登记;
//  生命周期为进程级静态(ZmHttpServer::Init/Open/Close),本类不再承载引用计数。
//  对外暴露三个服务器引用,由业务层(ServicePortal)注册全部路由。
//  设计:docs/designs/2026-08-30-drogon-httpserver-base-design.md §2.4
// ============================================================================

#include <memory>
#include <string>

#include "http_frontend_manager.h"
#include "http_jsonrpc_manager.h"
#include "http_restful_manager.h"

#include "zm_net_http_server.h"

/// exe 所在目录(www / certs 采用 exe 同级约定,结尾带路径分隔符)
std::string ZmExeDir();

class NetDock
{
public:
    NetDock() = default;
    ~NetDock() = default;

    // ── Phase1:全局 Init(静态,一次) + 构造/配置三面(全部先于 Open) ──
    /// @return 全局 Init + 三面 Init/Setup 是否全部成功
    bool Init();

    // ── Phase2/3:生命周期为静态全局一次(ZmHttpServer::Open/Close) ──
    /// @return 绑定失败(端口占用等)返回 false
    bool Open() { return ZmHttpServer::Open(); }
    void Close() { ZmHttpServer::Close(); }

    // ── 业务层路由注册入口(Phase1 内调用;设计 §2.4) ──
    ZmHttpFrontendServer* GetFrontendServer() { return m_frontend->GetServer(); }
    ZmHttpJsonRpcServer* GetJsonRpcServer() { return m_jrpc->GetServer(); }
    ZmHttpRestfulServer* GetRestfulServer() { return m_restful->GetServer(); }

    /// 证书热重载(FR-10;运行期唯一可热更新能力)
    bool ReloadCertificates();

private:
    std::unique_ptr<HttpFrontendManager> m_frontend;
    std::unique_ptr<HttpJsonRpcManager> m_jrpc;
    std::unique_ptr<HttpRestfulManager> m_restful;
};
