#pragma once
// ============================================================================
// HttpRestfulManager:RESTful 业务 API(39441)管理器
//  持有 ZmHttpRestfulServer 并负责配置;业务路由/WS/CORS 在业务层。
//  生命周期为进程级静态(ZmHttpServer::Init/Open/Close),本类不再委托 Open/Close。
//  分层:NetDock → HttpRestfulManager → ZmHttpRestfulServer(设计 §2.4)
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "zm_net_http_restful_server.h"
#include "service_define.h"

class HttpRestfulManager
{
public:
    HttpRestfulManager() = default;
    ~HttpRestfulManager() = default;

    /// @param useSSL   有全局证书时为 true(39441 升级 HTTPS,与前端共享证书)
    /// @param rootPath 业务根路径,默认 ZM_HTTP_RESTFUL_SERVER_ROOT_URI,可自定义
    void Init(uint16_t port = 39441, const std::string& ip = "0.0.0.0",
              bool useSSL = false,
              const std::string& rootPath = ZM_HTTP_RESTFUL_SERVER_ROOT_URI);
    void Setup();
    std::vector<uint16_t> GetPorts() const;

    ZmHttpRestfulServer* GetServer() const { return m_server.get(); }

private:
    std::unique_ptr<ZmHttpRestfulServer> m_server;
};
