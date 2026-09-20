#include "net_dock.h"

#include <windows.h>

#include <filesystem>

#include <zm_util_logger.h>

using std::string;

/**
 * @brief 取当前进程可执行文件所在目录(带尾部分隔符)
 *
 * 以宽字符 API 取路径后转窄串(当前按 ASCII 安装路径处理;非 ASCII 路径需改 UTF-8 转换)。
 *
 * @return 形如 "A:\ZiMo\svc\" 的目录串;取路径失败返回空串
 */
string ZmExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)   // 取路径失败或超出缓冲区
    {
        DEFAULT_LOG_ERROR("GetModuleFileNameW 失败");
        return {};
    }
    // 去掉文件名部分得到目录,并补齐尾部反斜杠(便于后续直接拼接子路径)
    string path = std::filesystem::path(std::wstring(buf)).parent_path().string();
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path += "\\";
    return path;
}

/**
 * @brief 网络层一次性初始化(全局参数/证书 + 三面配置 + 结构路由注册)
 *
 * 分三个子阶段:Phase1.1 全局 Init(参数/证书/全局 advice)、Phase1.2 三面构造与
 * 监听登记、Phase1.3 三面结构路由与门禁 advice 注册。全部先于 Open;调参改本函数,
 * 重启生效(运行期不可改)。
 *
 * @return true 成功;false 全局 Init 失败(调用方不应继续 Open)
 */
bool NetDock::Init()
{
    // 资源路径一律相对 exe 所在目录:静态资源 <exeDir>frontend、证书 <exeDir>certs
    std::string baseDir = ZmExeDir();
    std::string frontendRoot = baseDir + "frontend";
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
    opts.clientMaxBodySize = 10ULL * 1024 * 1024 * 1024;  // 单请求体上限 10GB(文件上传兜底),超限 413
    // 非流式路由请求体上限(PreRouting 按 Content-Length 预检 413);
    // 带 X-File-Size 声明者豁免(流式大上传路径);恶意声明 X-File-Size 仍受 clientMaxBodySize 兜底。
    opts.nonStreamBodyLimit = 256ULL * 1024 * 1024;
    // per-IP 连接数护栏(0 = 不限):⚠ 单机压测全部连接同源 IP,设值小于压测并发 → 被拒连;
    // 公网/防慢连接场景可设为 512~2048。
    opts.maxConnectionsPerIP = 0;
    // 指标端点:本项目不注册手写指标端点(RESTful 面的会话门禁会让监控抓不到 401)。
    //   需要监控时用 drogon 自带 utils/monitoring + PromExporter 插件(标准 Prometheus 文本 +
    //   label 维度);注意其注册的路由同样要落在某个面的 root 下,否则会在所有端口可达。
    opts.idleTimeoutSec = 90;              // keep-alive 空闲 90s 回收;调大可减少复用死连接型 NoHttpResponse
    opts.keepaliveRequests = 0;            // 单连接累计请求上限:0 = 不限次数回收(压测不触发次数回收竞态)
    opts.enableRequestStream = true;       // 上传流式落盘依赖,勿关
    opts.workPoolSize = 8;                 // 业务阻塞工作池线初始程数(DB/磁盘/CPU 型 handler),高并发可调大, 线程池中的线程动态扩张
    opts.gzipStatic = true;                // 静态 gzip:只找 <file>.gz 孪生优先发送(非现场压缩,补 Content-Encoding 头);
    bool hasCert = std::filesystem::exists(certFile) && std::filesystem::exists(keyFile);
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

    // CORS 白名单(业务策略,经 SetCorsAllowedOrigins 声明):
    //   本表是**跨站**许可表(Origin 全串匹配);**同站跨端口**(页面 80/443 → API 39441)
    //   由业务 CORS advice 硬放行,不经过此表;空表 = 只放行同站跨端口、跨站一律拒绝。
    //   部署域名(如 "https://www.xxx.com")在此登记;不含 "*"(与 Allow-Credentials 互斥)。
    ZmHttpServer::SetCorsAllowedOrigins({
        "http://localhost", "https://localhost",
        "http://127.0.0.1", "https://127.0.0.1"
    });

    // 自动 JSONP(目标形态):未声明路由不包装 —— 只有显式点名的路径可被跨站 <script>
    //   读取(JSONP 天然绕过 CORS,"哪条接口允许被任意站点读"必须逐条决策)。
    //   仅翻全局开关时其余字段沿用内置基线;声明须在 Open 前,与 CORS 白名单同批。
    ZmHttpServer::ZmJsonpOptions jsonp;
    jsonp.enabled = false;
    ZmHttpServer::SetJsonpDefaults(jsonp);
    ZmHttpServer::SetJsonpEnabled("/ping");   // 探针保留 JSONP(外部探活兼容)

    // ── Phase1.2:构造/配置三面(仅登记端口 + 文档根 + 根路径,不启动) ──
    m_frontend = std::make_unique<HttpFrontendManager>();
    m_jrpc = std::make_unique<HttpJsonRpcManager>();
    m_restful = std::make_unique<HttpRestfulManager>();

    m_frontend->Init("0.0.0.0", hasCert, frontendRoot);
    m_jrpc->Init(39440, "0.0.0.0", hasCert);
    m_restful->Init(39441, "0.0.0.0", hasCert);

    // 前端静态缓存头策略:默认"再校验态"(每次 IMS → 304);
    // 静态资源发布"指纹命名"(文件名带内容哈希)后,把 js/css 切"长缓存态":
    //   SetStaticCachePolicy({ ..., { ".js", "public, max-age=31536000, immutable" } })
    m_frontend->GetServer()->SetStaticCachePolicy(
        {"public, max-age=0, must-revalidate", {}});

    // 前端门禁的"外来前缀"来源:各面 SetRootPath 已登记进进程级归属表,前端门禁按归属
    // 自动 404,无需在此手工登记。AddOtherRootPath 是兜底口子(追加归属表
    // 之外的前缀),当前无此需要故不调用。

    // ── Phase1.3:结构路由(门禁/SPA/重定向 advice;先于任何 Open) ──
    m_frontend->Setup();
    m_jrpc->Setup();
    m_restful->Setup();

    DEFAULT_LOG_INFO("NetDock::Init 完成:前端[{}{}] JRPC[39440 {}] RESTful[39441 {}]",
                     hasCert ? "80+443 HTTPS" : "80 HTTP", "",
                     hasCert ? "HTTPS" : "HTTP", hasCert ? "HTTPS" : "HTTP");
    return true;
}

/**
 * @brief 热重载全局 TLS 证书
 *
 * 证书为进程级全局,重载与具体服务器面无关,故直接转发基类静态能力。
 *
 * @return true 重载成功;false 未配置证书或重载失败
 */
bool NetDock::ReloadCertificates()
{
    return ZmHttpServer::ReloadCertificates();
}
