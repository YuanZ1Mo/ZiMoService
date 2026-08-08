#include "service_portal.h"

#include "service_define.h"
#include "net_dock.h"
#include "broadcast_manager.h"
#include "http_server_manager.h"
#include "module_file_hub.h"
#include "module_server_audio_stream.h"
#include "module_deepseek.h"
#include "modules/module_user.h"
#include "modules/module_portal.h"

#include "zm_net_req_loop_protocol.h"   // ZmReqLoopRest(回复 helper 静态调用)+ ZmReqLoopPool
#include "zm_net_http.h"   // ZmHttpdTask/evhttp_cmd_type/ZmHttpUtil + ZmJsonRpcServer(直通回复与路由用)
#include "zm_net_socket.h" // ZmWinSockHelper(池预创建客户端前先完成 WSAStartup,防启动竞态)
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "zm_util_file.h"
#include "zm_util_str.h"

// ============================================================================
// 构造 / 析构
// ============================================================================

ServicePortal::ServicePortal()
{
	m_fileHubModule = new FileHubModule();
	m_audioModule = new ServerAudioStreamModule();

	// Winsock 先行初始化(幂等):池预创建客户端循环线程时 libevent 需已 WSAStartup,
	// 否则 event_base_new 失败(实测:池 init 报 client start failed,预创建全部落空)
	ZmWinSockHelper::Init();

	// 通用外呼请求池(全门户共用;预创建4,上限16)
	m_httpClientPool = new ZmHttpClientPool();
	if (!m_httpClientPool->Init(4, 16))
		DEFAULT_LOG_ERROR("ServicePortal: ZmHttpClientPool init failed");
	// DeepSeek 模块:池创建后注入(配置/缓存/usage 处理全部在模块内)
	m_deepseekModule = new DeepSeekModule(m_httpClientPool);
	// 用户系统:双库初始化(建表);失败仅记日志,服务继续(用户系统不可用)
	m_userModule = new UserModule();
	if (!m_userModule->Open())
		DEFAULT_LOG_ERROR("ServicePortal: UserModule init failed");
	// 门户模块:注入 UserModule(鉴权/角色/模块权限查询)
	m_portalModule = new PortalModule(m_userModule);
}

ServicePortal::~ServicePortal()
{
	delete m_audioModule;
	m_audioModule = nullptr;
	delete m_fileHubModule;
	m_fileHubModule = nullptr;

	if (m_httpClientPool)
	{
		delete m_httpClientPool;   // 先停客户端循环线程(join):在飞回调此时仍可安全查模块 gone 标志
		m_httpClientPool = nullptr;
	}
	// 池删后无新回调,模块方可安全释放(Shutdown 已置 gone;池 join 期间回调见 gone → 删 st 不投递)
	delete m_deepseekModule;
	m_deepseekModule = nullptr;

	delete m_portalModule;
	m_portalModule = nullptr;

	delete m_userModule;
	m_userModule = nullptr;
}

// ============================================================================
// 门户停止准备
// ============================================================================

void ServicePortal::Shutdown()
{
	// 业务层停止钩子:NetDock 析构前标记音频模块 task/loop 与 SSE 测试线程 loop 即将失效
	// (NetDock 析构触发 closecb,发送线程提前退出时须跳过流收尾投递,防 UAF)
	if (m_audioModule)
		m_audioModule->SetTasksGone();
	// DeepSeek 模块:回调经 m_gone 跳过续体投递(防 loop 已销毁)
	if (m_deepseekModule)
		m_deepseekModule->Shutdown();
	// 用户系统:停清理线程(join)+ 关双库
	if (m_userModule)
		m_userModule->Shutdown();
	m_sseGone.store(true);
}


// ============================================================================
// 广播消息便捷方法
// ============================================================================

bool ServicePortal::BroadcastMessage(const std::string& topic, const std::string& content, const std::string& tag)
{
	if (!m_netDock)
		return false;

	auto* mgr = m_netDock->GetBroadcastManager();
	if (!mgr)
		return false;

	return mgr->Broadcast(topic, content, tag);
}

// ============================================================================
// HTTP 80 端口路由注册
// ============================================================================

void ServicePortal::RegisterHttpRoutes(HttpServerManager* httpMgr)
{
	if (!httpMgr)
		return;

	auto& router = httpMgr->GetRouter();

	router.Get("/", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/index.html");
	});

	router.Get("/404", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/404.html");
	});

	// 兜底路由：未匹配的请求统一走 ServeStaticFile（文件不存在则展示 404 页面）
	router.Any("*", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		std::string uri(task->Uri() ? task->Uri() : "/");
		return httpMgr->ServeStaticFile(task, uri);
	});

	// 用户系统:模块自注册 /login /register /reset 静态页路由
	if (m_userModule)
		m_userModule->RegisterHttpRoutes(httpMgr);
	// 门户模块:自注册 /portal 与 /portal/* SPA fallback
	if (m_portalModule)
		m_portalModule->RegisterHttpRoutes(httpMgr);
}

// ============================================================================
// JRPC 请求回调
// ============================================================================

void ServicePortal::JrpcRequestReadCB(ZmReqLoop* loop, const char* reqData)
{
	ZMJSON rsp_result;
	ZMJSON rsp_error;
	std::string err;

	ZMJSON reqJson = zm_json_parse(reqData, err);
	if (!err.empty())
	{
		ZMJSON rsp;
		rsp["error"]["code"]    = -32700;
		rsp["error"]["message"] = "Parse error: " + err;
		ZmReqLoopJrpc::ResponseJson(loop, rsp);
		return;
	}

	std::string req_method = zm_json_get_str(reqJson, "method");
	ZMJSON req_params = reqJson["params"];

	if (req_method == "ping")
	{
		rsp_result["pong"] = true;
	}
	else
	{
		rsp_error["code"]    = -32601;
		rsp_error["message"] = "Method not found: " + req_method;
	}

	ZMJSON rsp;
	if (!rsp_error.empty())
		rsp["error"] = rsp_error;
	else
		rsp["result"] = rsp_result;
	ZmReqLoopJrpc::ResponseJson(loop, rsp);
}

// ============================================================================
// RESTful 请求回调
// ============================================================================

void ServicePortal::RestfulRequestCB(ZmReqLoop* loop,
	const BYTE* body, size_t body_len)
{
	auto* task = loop->Task();
	evhttp_cmd_type verb = task->Method();

	std::string path(task->Path() ? task->Path() : "/");

	// 剥掉根 URI 前缀（如 /zimo/api/ping → /ping），保持后续路由匹配不变
	{
		const char* root = ZM_HTTP_RESTFUL_SERVER_ROOT_URI;
		size_t rootLen = strlen(root);
		if (path.size() >= rootLen && path.compare(0, rootLen, root) == 0)
			path = path.substr(rootLen);
		if (path.empty()) path = "/";
	}

	auto qv = [&](const char* key, const char* def = "") {
		return std::string(task->GetQueryValue(key, def));
	};

	// ── 用户系统:auth 分发(命中即返回)────────────────────
	if (m_userModule && m_userModule->DispatchRest(loop, verb, path, task, body, body_len))
		return;
	// ── 门户模块:/portal/* 分发(命中即返回)──────────────
	if (m_portalModule && m_portalModule->DispatchRest(loop, verb, path, task, body, body_len))
		return;

	// ── GET /ping ────────────────────────────────────────
	if (verb == EVHTTP_REQ_GET && path == "/ping")
	{
		ZmReqLoopRest::ResponseJson(loop, 200, {{"pong", true}});
	}
	else { ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + std::string(ZmHttpUtil::VerbToString(verb)) + " " + path); }
}