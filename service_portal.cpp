#include "service_portal.h"

#include "service_define.h"
#include "net_dock.h"
#include "broadcast_manager.h"
#include "http_server_manager.h"
#include "module_file_hub.h"
#include "module_server_audio_stream.h"
#include "module_deepseek.h"

#include "zm_net_req_loop_protocol.h"   // ZmReqLoopJrpc/ZmReqLoopRest(回复 helper 静态调用)+ ZmReqLoopPool
#include "zm_net_http.h"   // ZmHttpdTask/evhttp_cmd_type/ZmHttpUtil(直通回复与路由用)
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

	router.Get("/control", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/control.html");
	});

	router.Get("/filehub", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/filehub.html");
	});

	router.Get("/audio", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/audio.html");
	});

	router.Get("/deepseek", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/deepseek.html");
	});

	router.Get("/404", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/404.html");
	});

	// 兜底路由：未匹配的请求统一走 ServeStaticFile（文件不存在则展示 404 页面）
	router.Any("*", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		std::string uri(task->Uri() ? task->Uri() : "/");
		return httpMgr->ServeStaticFile(task, uri);
	});
}

// ============================================================================
// JRPC 请求回调
// ============================================================================

void ServicePortal::JrpcRequestReadCB(ZmReqLoop* loop, const char* reqData)
{
	ZMJSON rsp_headers;
	ZMJSON rsp_result;
	ZMJSON rsp_error;
	std::string err;

	ZMJSON reqJson = zm_json_parse(reqData, err);
	if (!err.empty())
	{
		ZMJSON rsp;
		rsp["error"]["code"]    = -32700;
		rsp["error"]["message"] = "Parse error: " + err;
		ZmReqLoopJrpc::Response(loop, rsp);
		return;
	}

	std::string req_method = zm_json_get_str(reqJson, "method");
	ZMJSON req_params = reqJson["params"];
	ZMJSON req_headers = reqJson["headers"];

	if (req_method == "ping")
	{
		rsp_result["pong"] = true;
	}
	else if (req_method == "drop")
	{
		// ★ 先驱动 doer 回收(旧 tap->Drop 立即关连接;现在回复空 200 后关闭)
		if (loop->TryReply())   // ★ 已被他方收尾则跳过驱动(防双事件)
		{
			loop->Task()->SetReply(200);
			loop->Task()->TriggerReply();
		}
		loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, loop->Task());
		return;
	}
	else if (req_method == "getTime")
	{
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		rsp_result["time"] = buf;
		rsp_result["timestamp"] = (long)now;
	}
	else if (req_method == "getStatus")
	{
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		rsp_result["time"] = buf;
		rsp_result["timestamp"] = (long)now;

		rsp_result["http"]["status"] = (m_netDock && m_netDock->IsHttpOpen()) ? "running" : "stopped";
		rsp_result["http"]["port"]   = ZM_HTTP_SERVER_PORT;

		rsp_result["jrpc_http"]["status"] = (m_netDock && m_netDock->IsJrpcHttpOpen()) ? "running" : "stopped";
		rsp_result["jrpc_http"]["port"]   = ZM_JSONRPC_SERVER_PORT;

		rsp_result["broadcast"]["status"]      = (m_netDock && m_netDock->IsBroadcastOpen()) ? "running" : "stopped";
		rsp_result["broadcast"]["port"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetPort() : 0;
		rsp_result["broadcast"]["connections"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetConnectionCount() : 0;
		rsp_result["broadcast"]["sent"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetSentCount() : 0;
		auto load = ZmSystem::GetSystemLoad();
		rsp_result["system"]["cpu"]           = load.cpu_percent;
		rsp_result["system"]["memory"]        = load.memory_percent;
		rsp_result["system"]["totalMemMB"]    = load.total_memory_mb;
		rsp_result["system"]["usedMemMB"]     = load.used_memory_mb;
		rsp_result["system"]["gpuAvailable"]  = load.has_gpu;
		rsp_result["system"]["gpu"]           = (load.has_gpu ? load.gpu_percent : -1.0);
	}
	else if (req_method == "broadcast")
	{
		std::string topic   = zm_json_get_str(req_params, "topic", "");
		std::string content = zm_json_get_str(req_params, "content", "");
		std::string tag     = zm_json_get_str(req_params, "tag", "");

		if (topic.empty())
		{
			rsp_result["success"] = false;
			rsp_result["error"]   = "topic is required";
		}
		else
		{
			rsp_result["success"] = BroadcastMessage(topic, content, tag);
		}
	}
	else if (req_method == "echo")
	{
		rsp_result["echo"] = req_params;
	}
	// --- 文件中心 API ---
	else if (req_method == "listFiles")
	{
		std::string path = zm_json_get_str(req_params, "path", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->ListFiles(path)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "searchFiles")
	{
		std::string keyword = zm_json_get_str(req_params, "keyword", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->SearchFiles(keyword)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "createDir")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string dirName  = zm_json_get_str(req_params, "dirName", "");
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->CreateDir(path, dirName, username, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "deleteItem")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->DeleteItem(path, username, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "verifyDirPassword")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->VerifyDirPassword(path, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "changeDirPassword")
	{
		std::string path        = zm_json_get_str(req_params, "path", "");
		std::string username    = zm_json_get_str(req_params, "username", "");
		std::string oldPassword = zm_json_get_str(req_params, "oldPassword", "");
		std::string newPassword = zm_json_get_str(req_params, "newPassword", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->ChangeDirPassword(path, username, oldPassword, newPassword)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "batchDelete")
	{
		ZMJSON paths          = req_params["paths"];
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_fileHubModule ? m_fileHubModule->BatchDelete(paths, username, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "getRoutes")
	{
		ZMJSON arr = ZMJSON::array();

		auto add = [&](const char* m, const char* cat, const char* desc,
		               const char* reqEx, const char* rspEx) {
			ZMJSON item;
			item["method"]          = m;
			item["path"]            = cat;
			item["description"]     = desc;
			item["requestExample"]  = reqEx;
			item["responseExample"] = rspEx;
			arr.push_back(item);
		};

		add("ping",     "系统", "心跳检测，返回 pong",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{}}",
			"{\"result\":{\"pong\":true}}");
		add("getTime",  "系统", "获取服务器当前时间",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"getTime\",\"params\":{}}",
			"{\"result\":{\"time\":\"...\",\"timestamp\":0}}");
		add("getStatus","系统", "获取服务器综合状态",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"getStatus\",\"params\":{}}",
			"{\"result\":{\"http\":{\"status\":\"running\"}}}");
		add("broadcast","广播", "向所有匹配tag的客户端广播消息",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"broadcast\",\"params\":{\"topic\":\"hello\",\"content\":\"hello world\",\"tag\":\"demo\"}}",
			"{\"result\":{\"success\":true}}");
		add("echo",     "测试", "通用接口测试",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":{\"key\":\"value\"}}",
			"{\"result\":{\"echo\":{\"key\":\"value\"}}}");
		add("getRoutes","文档", "获取 JRPC 方法文档列表",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"getRoutes\",\"params\":{}}",
			"{\"result\":{\"routes\":[...],\"total\":14}}");
		add("getAbout", "文档", "获取后端和前端技术信息",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"getAbout\",\"params\":{}}",
			"{\"result\":{\"backend\":\"...\",\"frontend\":\"...\"}}");
		// 文件中心
		add("listFiles",  "文件中心", "列出目录下的文件和文件夹",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"listFiles\",\"params\":{\"path\":\"\"}}",
			"{\"result\":{\"ok\":true,\"files\":[...]}}");
		add("searchFiles","文件中心", "模糊搜索文件/文件夹",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"searchFiles\",\"params\":{\"keyword\":\"report\"}}",
			"{\"result\":{\"ok\":true,\"results\":[...]}}");
		add("createDir",  "文件中心", "新建目录（可选设密码）",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"createDir\",\"params\":{\"path\":\"\",\"dirName\":\"NewDir\",\"username\":\"\",\"password\":\"\"}}",
			"{\"result\":{\"ok\":true}}");
		add("deleteItem", "文件中心", "删除文件或空文件夹",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"deleteItem\",\"params\":{\"path\":\"dir/file.txt\"}}",
			"{\"result\":{\"ok\":true}}");
		add("verifyDirPassword","文件中心","验证目录密码",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"verifyDirPassword\",\"params\":{\"path\":\"ProtectedDir\",\"password\":\"123\"}}",
			"{\"result\":{\"ok\":true,\"valid\":true}}");
		add("changeDirPassword","文件中心","修改目录密码",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"changeDirPassword\",\"params\":{\"path\":\"Dir\",\"username\":\"alice\",\"oldPassword\":\"old\",\"newPassword\":\"new\"}}",
			"{\"result\":{\"ok\":true}}");
		add("batchDelete","文件中心","批量删除文件",
			"{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"batchDelete\",\"params\":{\"paths\":[\"a.txt\",\"b.txt\"]}}",
			"{\"result\":{\"ok\":true,\"deleted\":2}}");

		rsp_result["routes"] = arr;
		rsp_result["total"]  = (int)arr.size();
	}
	else if (req_method == "getAbout")
	{
		char moduleDir[MAX_PATH];
		std::string exeDir = ZmSystem::GetModuleDir(moduleDir, MAX_PATH);
		std::string root;
		if (!exeDir.empty())
		{
			char normalized[MAX_PATH];
			std::string upOne = exeDir + "\\..";
			if (GetFullPathNameA(upOne.c_str(), MAX_PATH, normalized, nullptr))
				root = normalized;
		}
		if (root.empty())
		{
			rsp_error["code"]    = -32603;
			rsp_error["message"] = "无法获取项目根目录";
		}
		else
		{
			std::string backendMd;
			if (ZmFile::ReadString((root + "\\README.md").c_str(), backendMd))
				rsp_result["backend"] = backendMd;
			else
				rsp_result["backend"] = "README.md not found (path: " + root + "\\README.md)";

			std::string frontendMd;
			if (ZmFile::ReadString((root + "\\www\\doc\\README.md").c_str(), frontendMd))
				rsp_result["frontend"] = frontendMd;
			else
				rsp_result["frontend"] = "www/doc/README.md not found (path: " + root + "\\www\\doc\\README.md)";
		}
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
	rsp["headers"] = rsp_headers;
	ZmReqLoopJrpc::Response(loop, rsp);
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

	// ── GET /ping ────────────────────────────────────────
	if (verb == EVHTTP_REQ_GET && path == "/ping")
	{
		ZmReqLoopRest::ResponseJson(loop, 200, {{"pong", true}});
	}
	// ── GET /time ────────────────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/time")
	{
		time_t now = time(nullptr); char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		ZmReqLoopRest::ResponseJson(loop, 200, {{"time", buf}, {"timestamp", (long)now}});
	}
	// ── GET /status ──────────────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/status")
	{
		time_t now = time(nullptr); char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		ZMJSON info;
		info["time"] = buf; info["timestamp"] = (long)now;
		info["http"]["status"] = (m_netDock && m_netDock->IsHttpOpen()) ? "running" : "stopped";
		info["http"]["port"] = ZM_HTTP_SERVER_PORT;
		info["jrpc_http"]["status"] = (m_netDock && m_netDock->IsJrpcHttpOpen()) ? "running" : "stopped";
		info["jrpc_http"]["port"] = ZM_JSONRPC_SERVER_PORT;
		info["restful_http"]["status"] = (m_netDock && m_netDock->IsRESTfulHttpOpen()) ? "running" : "stopped";
		info["restful_http"]["port"] = ZM_RESTFUL_SERVER_PORT;
		info["broadcast"]["status"] = (m_netDock && m_netDock->IsBroadcastOpen()) ? "running" : "stopped";
		info["broadcast"]["port"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetPort() : 0;
		info["broadcast"]["connections"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetConnectionCount() : 0;
		info["broadcast"]["sent"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetSentCount() : 0;
		auto load = ZmSystem::GetSystemLoad();
		info["system"]["cpu"] = load.cpu_percent;
		info["system"]["memory"] = load.memory_percent;
		info["system"]["totalMemMB"] = load.total_memory_mb;
		info["system"]["usedMemMB"] = load.used_memory_mb;
		info["system"]["gpuAvailable"] = load.has_gpu;
		info["system"]["gpu"] = (load.has_gpu ? load.gpu_percent : -1.0);
		ZmReqLoopRest::ResponseJson(loop, 200, info);
	}
	// ── GET /deepseek/usage ──────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/deepseek/usage")
	{
		if (m_deepseekModule && m_deepseekModule->HandleUsageRequest(loop))
			return;
		ZmReqLoopRest::ResponseError(loop, 404, "deepseek module not available");
		return;
	}
	// ── POST /broadcast ──────────────────────────────────
	else if (verb == EVHTTP_REQ_POST && path == "/broadcast")
	{
		std::string topic = qv("topic"), content = qv("content"), tag = qv("tag");
		if (topic.empty()) ZmReqLoopRest::ResponseError(loop, 400, "topic is required");
		else ZmReqLoopRest::ResponseJson(loop, 200, {{"success", BroadcastMessage(topic, content, tag)}});
	}
	// ── POST /echo ───────────────────────────────────────
	else if (verb == EVHTTP_REQ_POST && path == "/echo")
	{
		ZMJSON echoObj;
		const char* qs = task->QueryStr();
		if (qs && qs[0]) {
			std::string qstr(qs);
			size_t pos = 0;
			while (pos < qstr.size()) {
				size_t eq = qstr.find('=', pos), amp = qstr.find('&', eq);
				if (amp == std::string::npos) amp = qstr.size();
				if (eq != std::string::npos && eq < amp)
					echoObj[qstr.substr(pos, eq - pos)] = qstr.substr(eq + 1, amp - eq - 1);
				pos = amp + 1;
			}
		}
		ZmReqLoopRest::ResponseJson(loop, 200, {{"echo", echoObj}});
	}
	// ── GET /routes ─────────────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/routes")
	{
		ZMJSON arr = ZMJSON::array();
		auto add = [&](const char* m, const char* p, const char* desc,
			const char* reqEx, const char* rspEx) {
			arr.push_back({{"method", m}, {"path", p}, {"description", desc},
				{"requestExample", reqEx}, {"responseExample", rspEx}});
		};
		add("GET",  "/ping",          "心跳检测",          "GET /ping",              "{\"pong\":true}");
		add("GET",  "/time",          "服务器时间",        "GET /time",              "{\"time\":\"...\",\"timestamp\":0}");
		add("GET",  "/status",        "服务器综合状态",    "GET /status",            "{\"http\":{\"status\":\"running\"}}");
		add("GET",  "/routes",        "API 文档列表",      "GET /routes",            "{\"routes\":[...],\"total\":17}");
		add("GET",  "/about",         "技术信息",          "GET /about",             "{\"backend\":\"...\"}");
		add("POST", "/broadcast",     "广播消息",          "POST /broadcast?topic=hello&content=world", "{\"success\":true}");
		add("POST", "/echo",          "接口测试",          "POST /echo?key=val",     "{\"echo\":{\"key\":\"val\"}}");
		add("GET",  "/files",         "列出目录文件",      "GET /files?path=subdir", "{\"ok\":true,\"files\":[...]}");
		add("GET",  "/files/search",  "模糊搜索文件",      "GET /files/search?keyword=report", "{\"ok\":true,\"results\":[...]}");
		add("POST", "/files/dirs",    "创建目录",          "POST /files/dirs?path=&dirName=D&username=admin&password=123", "{\"ok\":true}");
		add("DELETE","/files",        "删除文件/目录",     "DELETE /files?path=f.txt&username=admin&password=123", "{\"ok\":true}");
		add("GET",  "/files/verify-password","验证目录密码","GET /files/verify-password?path=Dir&password=secret", "{\"ok\":true,\"valid\":true}");
		add("PUT",  "/files/password","修改目录密码",      "PUT /files/password?path=Dir&username=admin&oldPassword=old&newPassword=new", "{\"ok\":true}");
		add("POST", "/files/batch-delete","批量删除文件",  "POST /files/batch-delete?paths=a.txt,b.txt&username=admin&password=123", "{\"ok\":true,\"deleted\":2}");
		add("POST", "/files/upload",  "上传文件",          "POST /files/upload?path=f.txt (body=二进制)", "{\"ok\":true,\"size\":102400}");
		add("GET",  "/files/download","下载文件",          "GET /files/download?path=f.txt", "(二进制, Content-Disposition: attachment)");
		add("GET",  "/events",        "SSE 事件推送",      "GET /events",            "data: {\"id\":0,\"data\":\"event 0\"}\\n\\n");
		add("GET",  "/audio/stream", "远程音频流",         "GET /audio/stream", "(二进制帧: len(4B)+seq(4B)+Opus 20ms 帧, 48000Hz 立体声)");
		ZmReqLoopRest::ResponseJson(loop, 200, {{"routes", arr}, {"total", (int)arr.size()}});
	}
	// ── GET /about ──────────────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/about")
	{
		char moduleDir[MAX_PATH];
		std::string exeDir = ZmSystem::GetModuleDir(moduleDir, MAX_PATH), root;
		if (!exeDir.empty()) {
			char normalized[MAX_PATH];
			std::string upOne = exeDir + "\\..";
			if (GetFullPathNameA(upOne.c_str(), MAX_PATH, normalized, nullptr)) root = normalized;
		}
		ZMJSON rsp;
		if (!root.empty()) {
			std::string md;
			if (ZmFile::ReadString((root + "\\README.md").c_str(), md)) rsp["backend"] = md;
			if (ZmFile::ReadString((root + "\\www\\doc\\README.md").c_str(), md)) rsp["frontend"] = md;
		}
		ZmReqLoopRest::ResponseJson(loop, 200, rsp);
	}
	// ── FileHub 操作 ─────────────────────────────────────────
	else if (path == "/files" || path.find("/files/") == 0)
	{
		if (!m_fileHubModule) { ZmReqLoopRest::ResponseError(loop, 503, "文件中心未初始化"); return; }

		// GET /filespath=... → 列出文件
		if (verb == EVHTTP_REQ_GET && path == "/files")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->ListFiles(qv("path")));
		// GET /files/search?keyword=... → 搜索
		else if (verb == EVHTTP_REQ_GET && path == "/files/search")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->SearchFiles(qv("keyword")));
		// POST /files/dirs → 创建目录
		else if (verb == EVHTTP_REQ_POST && path == "/files/dirs")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->CreateDir(qv("path"), qv("dirName"), qv("username"), qv("password")));
		// DELETE /filespath=... → 删除
		else if (verb == EVHTTP_REQ_DELETE && path == "/files")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->DeleteItem(qv("path"), qv("username"), qv("password")));
		// GET /files/verify-password?path=...&password=...→ 验证密码
		else if ((verb == EVHTTP_REQ_GET || verb == EVHTTP_REQ_POST) && path == "/files/verify-password")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->VerifyDirPassword(qv("path"), qv("password")));
		// PUT /files/password → 修改密码
		else if (verb == EVHTTP_REQ_PUT && path == "/files/password")
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->ChangeDirPassword(qv("path"), qv("username"), qv("oldPassword"), qv("newPassword")));
		else if (verb == EVHTTP_REQ_POST && path == "/files/batch-delete")
		{
			std::string pathsStr = qv("paths");
			ZMJSON pathsArr = ZMJSON::array();
			if (!pathsStr.empty()) {
				size_t start = 0, end;
				while ((end = pathsStr.find(',', start)) != std::string::npos)
					{ pathsArr.push_back(pathsStr.substr(start, end - start)); start = end + 1; }
				pathsArr.push_back(pathsStr.substr(start));
			}
			ZmReqLoopRest::ResponseJson(loop, 200, m_fileHubModule->BatchDelete(pathsArr, qv("username"), qv("password")));
		}
		// POST /files/upload?path=... → 上传（body 是文件内容）
		else if (verb == EVHTTP_REQ_POST && path == "/files/upload")
		{
			std::string filePath = qv("path");
			if (filePath.empty()) { ZmReqLoopRest::ResponseError(loop, 400, "path is required"); return; }
			int code = m_fileHubModule->ReceiveFile(task, m_fileHubModule->GetHubRoot() + "\\" + filePath, body, body_len);
			ZmReqLoopRest::ResponseJson(loop, code, {{"ok", code < 400}, {"path", filePath}, {"size", body_len}});
		}
		// GET /files/download?path=... → 下载（支持断点续传）
		else if (verb == EVHTTP_REQ_GET && path == "/files/download")
		{
			std::string filePath = qv("path");
			if (filePath.empty()) { ZmReqLoopRest::ResponseError(loop, 400, "path is required"); return; }
			std::replace(filePath.begin(), filePath.end(), '/', '\\');
			std::string fullPath = m_fileHubModule->GetHubRoot() + "\\" + filePath;
			int code = m_fileHubModule->SendFile(task, fullPath);
			if (code < 400) {
				if (loop->TryReply())   // ★ 已被他方收尾则跳过驱动(防双事件)
					task->TriggerReply();
				// 原 tap->Drop():回复已投出,以 DONE 收尾(ProcessDone:TryReply+Release 回池)
				loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
			} else {
				ZmReqLoopRest::ResponseError(loop, code, code == 404 ? "文件不存在" : "下载失败");
			}
		}
		else { ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + std::string(ZmHttpUtil::VerbToString(verb)) + " " + path); }
	}
	// ── GET /events (SSE) ──────────────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/events")
	{
		ZmReqLoopRest::ResponseSSEStart(loop);
		ZmHttpdTask* sseTask = task;   // 捕获请求上下文;外部线程只碰 task 的线程安全接口
		std::thread([sseTask, loop, this] {
			for (int i = 0; i < 50; i++)
			{
				if (!sseTask->IsStreaming() || sseTask->IsConnClosed())
					break;   // 流未开/客户端已断:退出
				std::string line = "data: " + (ZMJSON{{"id", i}, {"data", "event " + std::to_string(i)}}).dump() + "\n\n";
				sseTask->SendReplyChunk((const BYTE*)line.c_str(), line.size());   // 线程安全
				Sleep(2000);
			}
			loop->TryReply();   // ★ 同上:防 CLOSE 竞态双驱动
			sseTask->EndStreamReply();   // 线程安全:驱动 doer 回收
			if (!m_sseGone.load())
				loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, sseTask);   // ★ ctx=task;陈旧投递由 ProcessDone 身份校验丢弃
		}).detach();
	}
	// ── GET /audio/stream (远程音频流) ─────────────────────
	else if (verb == EVHTTP_REQ_GET && path == "/audio/stream")
	{
		if (!m_audioModule)
		{
			ZmReqLoopRest::ResponseError(loop, 503, "音频服务未初始化");
			return;
		}
		// 先订阅(必要时启动采集):失败可真实返回 503(规格 §7 无设备场景)
		if (!m_audioModule->Subscribe(task, loop))
		{
			DEFAULT_LOG_WARN("[audio] 订阅失败:服务器无可用音频设备");
			ZmReqLoopRest::ResponseError(loop, 503, "服务器无音频输出设备");
			return;
		}
		ZmReqLoopRest::ResponseStreamStart(loop, 200, {
			{"Content-Type", "application/octet-stream"},
			{"Cache-Control", "no-cache"}
		});
		// M1:Subscribe 与 StartStreamReply 之间采集可能瞬时失败收尾、
		// 订阅者已被移除——此时无人发流,主动结束避免挂死的空流
		if (!m_audioModule->IsSubscriberAlive(task))
		{
			if (loop->TryReply())   // ★ 已被他方收尾则跳过驱动(防双事件)
				task->EndStreamReply();
			// 回调与发送线程可能双投 DONE/双 EndStreamReply——DONE 由 epoch/身份校验去重,
			// EndStreamReply 幂等,有意为之
			loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
		}
		// 订阅成功:发送由订阅者内部线程执行,本回调直接返回
	}
	else { ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + std::string(ZmHttpUtil::VerbToString(verb)) + " " + path); }
}