#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H

// ============================================================================
// ServicePortal:业务层门户(重建,Drogon 版)
//  负责全部路由注册与 handler 业务逻辑(用户:路由注册及相关业务逻辑全部放
//  到 service_portal 业务层处理)。本期仅注册测试接口,不对接历史业务能力;
//  业务期各业务模块再经 ServicePortal 回调注册。
// ============================================================================

#include <atomic>
#include <string>

class NetDock;
class ZmHttpFrontendServer;
class ZmHttpJsonRpcServer;
class ZmHttpRestfulServer;

class ServicePortal
{
public:
    /// @param netDock 网络层宿主(Phase1:NetDock::Init 后传入;本类存裸指针)
    explicit ServicePortal(NetDock* netDock);
    ~ServicePortal();

    /// Phase1:注册全部路由(前端页面别名 / JRPC / RESTful 测试组 / WS / CORS)
    /// 必须在 ZmHttpServer::Open() 前调用
    void Init();

    /// 业务收尾(Phase3 前调用;本期无业务线程,保留接口)
    void Shutdown();

    /// 广播消息(39640 自定义 TCP,本期不接入;保留签名,恒 false)
    bool BroadcastMessage(const std::string& topic, const std::string& content,
                          const std::string& tag);

private:
    void RegisterFrontendRoutes(ZmHttpFrontendServer* fe);
    void RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc);
    void RegisterRestfulTestRoutes(ZmHttpRestfulServer* rest);
    void RegisterRestfulCors(ZmHttpRestfulServer* rest);

    NetDock* m_netDock = nullptr;
    ZmHttpFrontendServer* m_frontend = nullptr;
    ZmHttpJsonRpcServer* m_jrpc = nullptr;
    ZmHttpRestfulServer* m_restful = nullptr;
};

#endif // SERVICE_PORTAL_H
