#pragma once
// ============================================================================
// HttpJsonRpcManager:JSON-RPC(39440)管理器
//  持有 ZmHttpJsonRpcServer 并负责监听配置;协议 handler 在业务层。
//  生命周期(Init/Open/Close)归 ZmHttpServer 的静态方法,本类只做"持有 + 配置"。
//  分层:NetDock → HttpJsonRpcManager → ZmHttpJsonRpcServer
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "zm_net_http_jsonrpc_server.h"
#include "service_define.h"

class HttpJsonRpcManager
{
public:
    HttpJsonRpcManager() = default;
    ~HttpJsonRpcManager() = default;

    /**
     * @brief 创建本面服务器并登记监听与业务根路径(Phase1)
     *
     * 只创建对象、登记监听与根路径,不启动服务(启动由 ZmHttpServer::Open 统一完成)。
     * 须在 Open 之前调用,且只调用一次。
     *
     * @param port     监听端口,默认 39440
     * @param ip       绑定地址(默认 0.0.0.0 通配)
     * @param useSSL   有全局证书时为 true(39440 走 HTTPS,与前端共享同一份证书)
     * @param rootPath 业务根路径,默认 ZM_HTTP_JRPC_SERVER_ROOT_URI,可自定义
     *
     * @example
     *   HttpJsonRpcManager mgr;
     *   mgr.Init(39440, "0.0.0.0", true);           // HTTPS 模式,默认根路径 /zimo/jrpc
     */
    void Init(uint16_t port = 39440, const std::string& ip = "0.0.0.0",
              bool useSSL = false,
              const std::string& rootPath = ZM_HTTP_JRPC_SERVER_ROOT_URI);

    /// 注册本面结构路由与门禁 advice(幂等;Phase1,须先于 Open)
    void Setup();

    /// @return 本面监听端口(未配置 = 0)
    uint16_t GetPort() const;

    ZmHttpJsonRpcServer* GetServer() const { return m_server.get(); }

private:
    std::unique_ptr<ZmHttpJsonRpcServer> m_server;
};
