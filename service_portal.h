#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H

// ============================================================================
// ServicePortal:业务层门户
//  负责路由注册与业务模块编排:前端页面路由 / JRPC / RESTful / CORS 在此登记,
//  各业务模块(modules 目录)由本类装配并注册自身接口
//  (模块划分见《2026-09-05-用户系统模块设计.md》)。
// ============================================================================

#include <atomic>
#include <memory>
#include <string>

class NetDock;
class ZmHttpFrontendServer;
class ZmHttpJsonRpcServer;
class ZmHttpRestfulServer;

class ZmDbModule;
class ZmUserModule;
class ZmPasswordModule;
class ZmSessionModule;
class ZmPermissionModule;
class ZmSecurityModule;
class ZmAuditModule;
class ZmAuthGateModule;
class ZmAuthModule;
class ZmUserAdminModule;
class ZmPortalModule;

// 文件中心
class ZmFileDbModule;
class ZmFileStoreModule;
class ZmFileAuditModule;
class ZmFileNodeModule;
class ZmFileTaskModule;
class ZmFileUploadModule;
class ZmFileTokenModule;
class ZmFilePackModule;
class ZmFileShareModule;
class ZmFileHubModule;
class ZmFileAdminModule;
class ZmDirLock;

class ServicePortal
{
public:
    /**
     * @brief 构造门户并绑定网络层宿主
     *
     * 只保存宿主指针(不持有所有权);真正的服务器引用与业务模块在 Init() 内建立。
     *
     * @param netDock 网络层宿主(须已完成 NetDock::Init,即三面已配置好)
     */
    explicit ServicePortal(NetDock* netDock);
    ~ServicePortal();

    /**
     * @brief 装配业务模块并注册全部路由与横切 advice(Phase1,须先于 Open)
     *
     * 顺序:创建业务模块(建库建表与种子)→ 前端面路由/门禁 → JRPC → RESTful 门禁与
     * 业务接口 → CORS 预检与响应头。任一步失败只记日志,不中断后续注册。
     */
    void Init();

    /// @brief 业务收尾(Phase3 之前调用:先收业务线程,再 Close 网络层)
    void Shutdown();

    /**
     * @brief 向 39640 广播端口发送消息
     *
     * 广播服务(自定义 TCP)当前未接入,固定返回 false。
     *
     * @param topic   主题
     * @param content 消息内容
     * @param tag     标签
     * @return 恒为 false(未接入)
     */
    bool BroadcastMessage(const std::string& topic, const std::string& content,
                          const std::string& tag);

private:
    /// 创建数据库模块与全部业务模块(依赖注入;建库建表 + 种子)
    void CreateModules();
    /// 注册前端面路由与页面门禁 advice
    void RegisterFrontendRoutes(ZmHttpFrontendServer* fe);
    /// 注册 JSON-RPC 面的业务 method 处理(当前无业务接口)
    void RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc);
    /// 注册 RESTful 面门禁 advice 与各业务模块接口
    void RegisterRestfulRoutes(ZmHttpRestfulServer* rest);
    /// 注册 CORS 预检(PreRouting)与响应头回显(PreSending)
    void RegisterRestfulCors(ZmHttpRestfulServer* rest);

    NetDock* m_netDock = nullptr;
    ZmHttpFrontendServer* m_frontend = nullptr;
    ZmHttpJsonRpcServer* m_jrpc = nullptr;
    ZmHttpRestfulServer* m_restful = nullptr;

    // 业务模块(生命周期随 ServicePortal;依赖方向:编排模块 → 数据服务模块 → DbModule)
    std::unique_ptr<ZmDbModule> m_db;
    std::unique_ptr<ZmUserModule> m_user;
    std::unique_ptr<ZmPasswordModule> m_password;
    std::unique_ptr<ZmSessionModule> m_session;
    std::unique_ptr<ZmPermissionModule> m_permission;
    std::unique_ptr<ZmSecurityModule> m_security;
    std::unique_ptr<ZmAuditModule> m_audit;
    std::unique_ptr<ZmAuthGateModule> m_gate;
    std::unique_ptr<ZmAuthModule> m_auth;
    std::unique_ptr<ZmUserAdminModule> m_admin;
    std::unique_ptr<ZmPortalModule> m_portal;

    // 文件中心(数据访问 → 存储 → 条目/任务 → 上传/打包/令牌/分享 → 编排)
    std::unique_ptr<ZmFileDbModule> m_fileDb;
    std::unique_ptr<ZmFileStoreModule> m_fileStore;
    std::unique_ptr<ZmFileAuditModule> m_fileAudit;
    std::unique_ptr<ZmDirLock> m_dirLock;
    std::unique_ptr<ZmFileTaskModule> m_fileTask;
    std::unique_ptr<ZmFileNodeModule> m_fileNode;
    std::unique_ptr<ZmFileUploadModule> m_fileUpload;
    std::unique_ptr<ZmFileTokenModule> m_fileToken;
    std::unique_ptr<ZmFilePackModule> m_filePack;
    std::unique_ptr<ZmFileShareModule> m_fileShare;
    std::unique_ptr<ZmFileHubModule> m_fileHub;
    std::unique_ptr<ZmFileAdminModule> m_fileAdmin;
};

#endif // SERVICE_PORTAL_H
