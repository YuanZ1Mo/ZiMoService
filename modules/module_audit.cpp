#include "modules/module_audit.h"

#include "modules/module_db.h"

// ============================================================================
// 构造
// ============================================================================
ZmAuditModule::ZmAuditModule(ZmDbModule* db)
    : m_db(db)
{
}

ZmAuditModule::~ZmAuditModule() = default;

drogon::Task<bool> ZmAuditModule::RecordOperation(int64_t operatorUid,
                                                  const std::string& operatorAccount,
                                                  const std::string& action,
                                                  const std::string& targetType,
                                                  int64_t targetId,
                                                  const std::string& detail,
                                                  const std::string& ip)
{
    co_return co_await m_db->Exec(
        "INSERT INTO operation_logs(operator_uid, operator_account, action, "
        "target_type, target_id, detail, ip, create_time) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
        {std::to_string(operatorUid), operatorAccount, action, targetType,
         std::to_string(targetId), detail, ip, std::to_string(ZmDbModule::Now())});
}

drogon::Task<bool> ZmAuditModule::RecordLogin(int64_t uid, const std::string& account,
                                              int result, const std::string& failReason,
                                              const std::string& ip,
                                              const std::string& ua)
{
    co_return co_await m_db->Exec(
        "INSERT INTO login_logs(uid, account, result, fail_reason, ip, ua, create_time) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7);",
        {std::to_string(uid > 0 ? uid : 0), account, std::to_string(result),
         failReason, ip, ua, std::to_string(ZmDbModule::Now())});
}
