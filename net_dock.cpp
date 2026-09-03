#include "net_dock.h"

#include <windows.h>

#include <filesystem>

#include <zm_util_logger.h>

using std::string;

// ----------------------------------------------------------------------------
/// exe 所在目录(www / certs 采用 exe 同级约定;服务加载 exe 同级 www)
// ----------------------------------------------------------------------------
string ZmExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
    {
        DEFAULT_LOG_ERROR("GetModuleFileNameW 失败");
        return {};
    }
    // ASCII 安装路径(A:\ZiMo\...);如遇非 ASCII 后续可换 UTF-8 转换
    string path = std::filesystem::path(std::wstring(buf)).parent_path().string();
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path += "\\";
    return path;
}

bool NetDock::Init()
{
    // 资源路径遵循 exe 同级约定(www / certs,记忆:服务加载 exe 同级 www)
    std::string baseDir = ZmExeDir();
    std::string wwwRoot = baseDir + "www";
    std::string certFile = baseDir + "certs\\server.crt";
    std::string keyFile = baseDir + "certs\\server.key";
    DEFAULT_LOG_INFO("OnStart: 构造 NetDock baseDir={}", baseDir);

    // ── Phase1.1:进程级全局 Init(静态,一次):全局参数/证书/全局 advice ──
    // 调参改这里,重启生效;每项含义/影响/特殊值见 zm_net_http_server.h Options 注释。
    ZmHttpServer::Options opts;
    opts.threadNum = 0;                    // 事件循环线程数:0 = 自动 = CPU 核数(吞吐核心)
    opts.maxConnections = 8192;            // 最大连接数护栏,超限拒连;也是峰值内存上限之一
    // 框架级请求体上限:1.9.13 对【全体请求含流式】强制(HttpRequestParser 层 413),
    // 是流式大上传(RegisterStreamCoro)唯一的框架兜底;勿调小于业务最大上传。
    // 非流式路由的提前拒绝由 nonStreamBodyLimit 承担。
    opts.clientMaxBodySize = 10ULL * 1024 * 1024 * 1024;  // 单请求体上限 10GB(文件上传兜底,FR-15),超限 413
    // 非流式路由请求体上限(PreRouting 按 Content-Length 预检 413);
    // 带 X-File-Size 声明者豁免(流式大上传路径);恶意声明 X-File-Size 仍受 clientMaxBodySize 兜底。
    opts.nonStreamBodyLimit = 256ULL * 1024 * 1024;
    // per-IP 连接数护栏(0 = 不限):⚠ 单机压测全部连接同源 IP,设值小于压测并发 → 被拒连;
    // 公网/防慢连接场景可设为 512~2048。
    opts.maxConnectionsPerIP = 0;
    opts.idleTimeoutSec = 90;              // keep-alive 空闲 90s 回收;调大可减少复用死连接型 NoHttpResponse
    opts.keepaliveRequests = 0;            // 单连接累计请求上限:0 = 不限次数回收(压测不触发次数回收竞态)
    opts.enableRequestStream = true;       // 上传流式落盘依赖,勿关
    opts.workPoolSize = 8;                 // 业务阻塞工作池线程数(DB/磁盘/CPU 型 handler),高并发可调大
    opts.gzip = false; opts.brotli = false;        // 动态压缩(CPU 换带宽):压测/延迟敏感保持关闭
    // 静态 gzip:只找 <file>.gz 孪生优先发送(非现场压缩,补 Content-Encoding 头);
    //   部署时跑一次 tools/build_www_gzip.sh 生成孪生;无孪生时本开关无副作用(照发原文件)。
    // 静态 brotli 同理但需 <file>.br 孪生,暂未维护,保持关闭。
    opts.gzipStatic = true;
    opts.brotliStatic = false;                     // 静态文件压缩开关(FR-18)
    bool hasCert = std::filesystem::exists(certFile) && std::filesystem::exists(keyFile);
    // CORS 白名单(仅回显名单内 Origin 的跨域响应;空 = 默认拒绝一切跨域):
    // 示例:opts.corsAllowedOrigins = { "https://www.example.com", "http://localhost:5173" };
    if (hasCert)
    {
        opts.certFile = certFile;        // 有全局证书 → 前端 443+80、JRPC/RESTful 同升 HTTPS
        opts.keyFile = keyFile;
    }
    if (!ZmHttpServer::Init(opts))
    {
        DEFAULT_LOG_ERROR("NetDock::Init 失败:ZmHttpServer::Init 返回 false");
        return false;
    }

    // ── Phase1.2:构造/配置三面(仅登记端口 + 文档根 + 根路径,不启动) ──
    m_frontend = std::make_unique<HttpFrontendManager>();
    m_jrpc = std::make_unique<HttpJsonRpcManager>();
    m_restful = std::make_unique<HttpRestfulManager>();

    m_frontend->Init("0.0.0.0", hasCert, wwwRoot);
    m_jrpc->Init(39440, "0.0.0.0", hasCert);
    m_restful->Init(39441, "0.0.0.0", hasCert);

    // 前端面门禁登记其他面业务根路径(自定义后仍正确拒绝外来前缀,设计 §4.4)
    m_frontend->GetServer()->AddOtherRootPath(m_jrpc->GetServer()->GetRootPath());
    m_frontend->GetServer()->AddOtherRootPath(m_restful->GetServer()->GetRootPath());

    // ── Phase1.3:结构路由(门禁/SPA/重定向 advice;先于任何 Open) ──
    m_frontend->Setup();
    m_jrpc->Setup();
    m_restful->Setup();

    DEFAULT_LOG_INFO("NetDock::Init 完成:前端[{}{}] JRPC[39440 {}] RESTful[39441 {}]",
                     hasCert ? "80+443 HTTPS" : "80 HTTP", "",
                     hasCert ? "HTTPS" : "HTTP", hasCert ? "HTTPS" : "HTTP");
    return true;
}

bool NetDock::ReloadCertificates()
{
    // 证书为进程级全局,热重载是基类静态能力(不依赖任何服务器面实例)
    return ZmHttpServer::ReloadCertificates();
}
