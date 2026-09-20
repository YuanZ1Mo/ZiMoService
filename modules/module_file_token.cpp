#include "modules/module_file_token.h"

#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_node.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <random>

using namespace drogon;

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileTokenModule::ZmFileTokenModule(ZmFileStoreModule* store, ZmFileNodeModule* node,
                                     ZmFileTaskModule* task)
    : m_store(store), m_node(node), m_task(task)
{
}

ZmFileTokenModule::~ZmFileTokenModule() = default;

// ============================================================================
// 令牌生成与编解码
// ============================================================================
std::string ZmFileTokenModule::NewToken()
{
    // 32 字符高熵随机:不承载任何 id、不可枚举、不可反推
    static const char*                 hex = "0123456789abcdef";
    std::random_device                 rd;
    std::mt19937_64                    gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string                        s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i)
        s += hex[dist(gen)];
    return s;
}

std::string ZmFileTokenModule::UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        // 保留 RFC 3986 未保留字符,其余一律百分号编码
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string ZmFileTokenModule::UrlDecode(const std::string& s)
{
    auto hexVal = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+')
            out += ' ';
        else
            out += s[i];
    }
    return out;
}

std::string ZmFileTokenModule::BuildUrl(const std::string& base, const std::string& token,
                                        const std::string& name)
{
    return base + "/zimo/api/filehub/dl/" + token + "/" + UrlEncode(name);
}

// ============================================================================
// 签发
// ============================================================================
drogon::Task<ZMJSON> ZmFileTokenModule::IssueNode(int64_t uid, int64_t nodeId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, uid, nodeId]() -> ZMJSON
        {
            ZMJSON row;
            if (!m_node->VisibleSync(nodeId, row))
                return ZmFileError(zm_file_err::kFileNotFound, 404, "文件不存在");
            if (zm_file_row_int(row, "type", 0) != zm_file::kTypeFile)
                return ZmFileError(zm_file_err::kBadRequest, 400, "目录请走打包下载");
            int64_t space = zm_file_row_int(row, "space", 0);
            // 令牌只签发给"本人可写空间"里的文件:否则拿到 id 就能换直链下载他人文件
            if (!ZmFileNodeModule::SpaceWritable(space, uid))
                return ZmFileError(zm_file_err::kFileNotFound, 404, "文件不存在");
            std::string   path;
            int64_t       sp = space;
            ZmStoreResult pr = m_store->PhysicalPathSync(nodeId, sp, path);
            if (!pr.ok)
                return ZmFileError(zm_file_err::kFileNotFound, 404, "文件不存在");

            Rec rec;
            rec.kind   = ZmTokenKind::Node;
            rec.uid    = uid;
            rec.nodeId = nodeId;
            rec.path   = path;
            rec.name   = zm_file_row_str(row, "name");
            rec.expire = ZmSqliteDb::Now() + zm_file::kTokenTtlSec;

            ZMJSON                      out = ZMJSON::object();
            std::lock_guard<std::mutex> lk(m_mtx);
            std::string                 token = NewToken();
            m_tokens[token]                   = rec;
            out["token"]                      = token;
            out["expire_time"]                = rec.expire;
            out["name"]                       = rec.name;
            return out;
        });
}

drogon::Task<ZMJSON> ZmFileTokenModule::IssueShare(const std::string& shareToken,
                                                   int64_t            nodeId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, shareToken, nodeId]() -> ZMJSON
        {
            ZMJSON row;
            if (!m_node->VisibleSync(nodeId, row))
                return ZmFileError(zm_file_err::kFileNotFound, 404, "文件不存在");
            if (zm_file_row_int(row, "type", 0) != zm_file::kTypeFile)
                return ZmFileError(zm_file_err::kBadRequest, 400, "目录请走打包下载");
            int64_t     space = zm_file_row_int(row, "space", 0);
            std::string path;
            int64_t     sp = space;
            if (!m_store->PhysicalPathSync(nodeId, sp, path).ok)
                return ZmFileError(zm_file_err::kFileNotFound, 404, "文件不存在");

            Rec rec;
            rec.kind       = ZmTokenKind::Share;
            rec.uid        = 0;
            rec.nodeId     = nodeId;
            rec.shareToken = shareToken;
            rec.path       = path;
            rec.name       = zm_file_row_str(row, "name");
            rec.expire     = ZmSqliteDb::Now() + zm_file::kTokenTtlSec;

            ZMJSON                      out = ZMJSON::object();
            std::lock_guard<std::mutex> lk(m_mtx);
            std::string                 token = NewToken();
            m_tokens[token]                   = rec;
            out["token"]                      = token;
            out["expire_time"]                = rec.expire;
            out["name"]                       = rec.name;
            return out;
        });
}

drogon::Task<ZMJSON> ZmFileTokenModule::IssuePack(int64_t uid, const std::string& taskNo)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, uid, taskNo]() -> ZMJSON
        {
            if (!m_task)
                return ZmFileError(zm_file_err::kInternal, 500, "任务模块不可用");
            ZMJSON row = m_task->TaskRowSync(taskNo);
            if (row.empty() || zm_file_row_int(row, "uid", 0) != uid)
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "压缩包不存在或已清理");
            // 压缩包相对空间缓存目录的路径记在 result(打包完成时写入)
            std::string rel   = zm_file_row_str(row, "result");
            int64_t     space = zm_file_row_int(row, "space", 0);
            if (rel.empty())
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "压缩包不存在或已清理");
            std::string path = m_store->ZipDir(space) + "\\" + rel;
            if (!m_store->Exists(path))
                return ZmFileError(zm_file_err::kNodeNotFound, 404, "压缩包不存在或已清理");

            Rec rec;
            rec.kind   = ZmTokenKind::Pack;
            rec.uid    = uid;
            rec.nodeId = 0;
            rec.taskNo = taskNo;
            rec.path   = path;
            rec.name   = zm_file_row_str(row, "name");
            rec.expire = ZmSqliteDb::Now() + zm_file::kTokenTtlSec;

            ZMJSON                      out = ZMJSON::object();
            std::lock_guard<std::mutex> lk(m_mtx);
            std::string                 token = NewToken();
            m_tokens[token]                   = rec;
            out["token"]                      = token;
            out["expire_time"]                = rec.expire;
            out["name"]                       = rec.name;
            return out;
        });
}

// ============================================================================
// 校验
// ============================================================================
bool ZmFileTokenModule::Find(const std::string& token, Rec& out)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto                        it = m_tokens.find(token);
    if (it == m_tokens.end())
        return false;
    if (it->second.expire < ZmSqliteDb::Now())
    {
        m_tokens.erase(it);
        return false;
    }
    out = it->second;
    return true;
}

int ZmFileTokenModule::AcquireSlot(const std::string& token, Rec& rec, uint64_t& slotOut)
{
    // 令牌级与全局闸门(全局并发直链下载必须有上限,
    // 否则事件循环上的分块读盘会把所有接口一起拖慢)
    if (rec.conns >= zm_file::kTokenMaxConn)
        return 503;
    if (m_activeDownloads >= zm_file::kGlobalDownloadMax)
        return 503;
    ++rec.conns;
    ++m_activeDownloads;
    // 每个占用登记一条在途名额:归还按句柄一一对应,
    // 同一令牌的多条连接各有各的句柄,不会互相顶替
    Slot s;
    s.token          = token;
    s.issuedAt       = ZmSqliteDb::Now();
    slotOut          = ++m_slotSeq;
    m_slots[slotOut] = std::move(s);
    return 0;
}

void ZmFileTokenModule::ReleaseSlotLocked(uint64_t slotId)
{
    auto slot = m_slots.find(slotId);
    if (slot == m_slots.end())
        return; // 已归还:重复调用到此为止,计数不会多减
    const std::string token = slot->second.token;
    m_slots.erase(slot);
    auto it = m_tokens.find(token);
    if (it != m_tokens.end() && it->second.conns > 0)
        --it->second.conns;
    if (m_activeDownloads > 0)
        --m_activeDownloads;
}

void ZmFileTokenModule::ReleaseSlot(uint64_t slotId)
{
    if (slotId == 0)
        return;
    std::lock_guard<std::mutex> lk(m_mtx);
    ReleaseSlotLocked(slotId);
}

void ZmFileTokenModule::TrackTransfer(uint64_t slotId,
                                      const std::weak_ptr<trantor::TcpConnection>& conn)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_slots.find(slotId);
    if (it != m_slots.end())
        it->second.conn = conn;
}

void ZmFileTokenModule::SweepLeaks(int64_t now)
{
    // 兜底:客户端在"响应开始发送"之前就断开时,平台的发送结束回调不会被触发
    // (框架在发送阶段才创建流),此时按"连接已消失"回收名额。
    // 只看连接存活、不按时间判:大文件传输会远超令牌有效期,按时间判会放掉在途名额
    std::vector<uint64_t> dead;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (const auto& kv : m_slots)
        {
            const Slot& s = kv.second;
            // 空登记(Resolve 完成、连接还没挂上)给 5 秒宽限,避免刚占用就被回收
            if (s.conn.expired() && (now - s.issuedAt) > 5)
                dead.push_back(kv.first);
        }
    }
    for (uint64_t id : dead)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_slots.find(id);
        if (it == m_slots.end())
            continue;
        DEFAULT_LOG_WARN("ZmFileTokenModule: 回收泄漏的下载名额 slot={} token={}", id,
                         it->second.token.substr(0, 8));
        ReleaseSlotLocked(id);
    }
}

drogon::Task<ZmTokenTarget> ZmFileTokenModule::Resolve(const std::string& token)
{
    ZmTokenTarget t;
    if (token.empty() || token.size() > 64)
    {
        t.status  = 410;
        t.code    = zm_file_err::kTokenExpired;
        t.message = "下载链接已失效,请重新发起下载";
        co_return t;
    }
    Rec rec;
    if (!Find(token, rec))
    {
        t.status  = 410;
        t.code    = zm_file_err::kTokenExpired;
        t.message = "下载链接已失效,请重新发起下载";
        co_return t;
    }

    // 目标复查:令牌只授权"下载这一个条目",是否仍然可见在此确认
    if (rec.kind == ZmTokenKind::Node)
    {
        ZMJSON row;
        bool   visible = co_await ZmHttpServer::RunOnPool<bool>(
            [this, &row, &rec]() -> bool { return m_node->VisibleSync(rec.nodeId, row); });
        if (!visible)
        {
            t.status  = 404;
            t.code    = zm_file_err::kFileNotFound;
            t.message = "文件已被删除";
            co_return t;
        }
        if (zm_file_row_int(row, "type", 0) != zm_file::kTypeFile)
        {
            t.status  = 410;
            t.code    = zm_file_err::kTokenExpired;
            t.message = "下载目标已变更,请重新发起下载";
            co_return t;
        }
        // 防交错:令牌只认签发时那一份文件。改名/移动会改变物理路径,若仍按冻结路径
        // 发送,发出去的可能是"换了名字的另一份内容" —— 路径一变即判令牌失效
        std::string nowPath;
        bool        moved = co_await ZmHttpServer::RunOnPool<bool>(
            [this, &rec, &nowPath]() -> bool
            {
                int64_t       sp = 0;
                ZmStoreResult pr = m_store->PhysicalPathSync(rec.nodeId, sp, nowPath);
                return !pr.ok || nowPath != rec.path;
            });
        if (moved)
        {
            t.status  = 410;
            t.code    = zm_file_err::kTokenExpired;
            t.message = "下载目标已变更,请重新发起下载";
            co_return t;
        }
    }
    else if (rec.kind == ZmTokenKind::Pack)
    {
        bool exists = co_await ZmHttpServer::RunOnPool<bool>(
            [this, &rec]() -> bool { return m_store->Exists(rec.path); });
        if (!exists)
        {
            t.status  = 404;
            t.code    = zm_file_err::kNodeNotFound;
            t.message = "压缩包不存在或已清理";
            co_return t;
        }
    }
    else // Share:分享有效性由分享模块在签发时校验,这里复查文件是否还在
    {
        ZMJSON row;
        bool   visible = co_await ZmHttpServer::RunOnPool<bool>(
            [this, &row, &rec]() -> bool { return m_node->VisibleSync(rec.nodeId, row); });
        if (!visible)
        {
            t.status  = 404;
            t.code    = zm_file_err::kShareUnavailable;
            t.message = "分享内容暂不可用";
            co_return t;
        }
    }

    int      gate = 0;
    uint64_t slot = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto                        it = m_tokens.find(token);
        if (it == m_tokens.end())
        {
            t.status  = 410;
            t.code    = zm_file_err::kTokenExpired;
            t.message = "下载链接已失效,请重新发起下载";
            co_return t;
        }
        gate = AcquireSlot(token, it->second, slot);
        // 名额占用即登记在途(此处还没有连接,由 TrackTransfer 补挂):
        // 正常由"发送结束回调"归还,异常路径由 SweepLeaks 兜底。
        // 这样闸门约束的是**在途传输**,而不是"处理中的请求"
    }
    if (gate != 0)
    {
        t.status  = gate;
        t.code    = "SERVER_BUSY";
        t.message = "下载连接数过多,请稍后重试";
        co_return t;
    }

    t.ok         = true;
    t.kind       = rec.kind;
    t.path       = rec.path;
    t.name       = rec.name;
    t.nodeId     = rec.nodeId;
    t.uid        = rec.uid;
    t.shareToken = rec.shareToken;
    t.taskNo     = rec.taskNo;
    t.slotId     = slot;
    t.fileSize   = co_await ZmHttpServer::RunOnPool<int64_t>(
        [this, &rec]() -> int64_t { return m_store->FileSize(rec.path); });
    co_return t;
}

void ZmFileTokenModule::RevokeByPack(const std::string& taskNo)
{
    // 只摘令牌:在途传输的名额由各自的结束回调归还(句柄在,不会漏)
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto it = m_tokens.begin(); it != m_tokens.end();)
    {
        if (it->second.kind == ZmTokenKind::Pack && it->second.taskNo == taskNo)
            it = m_tokens.erase(it);
        else
            ++it;
    }
}

void ZmFileTokenModule::EraseLocked(const std::string& token)
{
    m_tokens.erase(token);
}
