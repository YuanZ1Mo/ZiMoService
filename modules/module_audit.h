#ifndef ZM_MODULE_AUDIT_H
#define ZM_MODULE_AUDIT_H

// ============================================================================
// ZmAuditModule:审计模块(设计文档 §3.7)
//  数据归属:operation_logs(管理操作审计)、login_logs(登录成功/失败日志)。
// ============================================================================

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

class ZmDbModule;

class ZmAuditModule
{
public:
    explicit ZmAuditModule(ZmDbModule* db);
    ~ZmAuditModule();

    /// 管理操作审计(操作者 uid+account 快照、动作、目标类型/id、变更详情 JSON、ip)
    drogon::Task<bool> RecordOperation(int64_t operatorUid,
                                       const std::string& operatorAccount,
                                       const std::string& action,
                                       const std::string& targetType,
                                       int64_t targetId, const std::string& detail,
                                       const std::string& ip);
    /// 登录日志(result 1=成功 2=失败)
    drogon::Task<bool> RecordLogin(int64_t uid, const std::string& account,
                                   int result, const std::string& failReason,
                                   const std::string& ip, const std::string& ua);

private:
    ZmDbModule* m_db = nullptr;
};

#endif // ZM_MODULE_AUDIT_H
