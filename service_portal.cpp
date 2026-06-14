#include "service_portal.h"
#include "net_dock.h"
#include "http_server_manager.h"
#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "service_define.h"

#include <fstream>
#include <sstream>
#include <ctime>


// ============================================================================
// HTTP 80 端口路由注册
// ============================================================================

/**
 * @brief 注册 HTTP 80 端口精确路由
 *
 * 页面入口显式注册，静态资源按目录放行（/html/* /css/* /js/*）。
 * 未放行目录（如 doc/）无法通过 HTTP 访问。
 */
void ServicePortal::RegisterHttpRoutes(HttpServerManager* httpMgr)
{
	if (!httpMgr)
		return;

	auto& router = httpMgr->GetRouter();

	// GET / → 着陆页
	router.Get("/", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/index.html");
	});

	// GET /control → 控制中心 SPA
	router.Get("/control", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
		return httpMgr->ServeStaticFile(task, "/html/control.html");
	});
}

// ============================================================================
// JRPC 方法注册
// ============================================================================

void ServicePortal::RegJrpcMethod(const char* name, const char* category,
                                   const char* description,
                                   const char* requestExample, const char* responseExample,
                                   JrpcHandler handler)
{
	m_jrpcHandlers[name] = handler;
	m_jrpcMethods.push_back({ name, category, description, requestExample, responseExample });
}

/**
 * @brief 获取 exe 所在目录的上层目录路径
 *
 * exe 位于 $(SolutionDir)$(Configuration)\，上层即 $(SolutionDir)。
 * 用于定位 README.md 和 www/ 等源码目录下的文件。
 */
static std::string GetProjectRoot()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	std::string dir(exePath);
	size_t pos = dir.find_last_of("\\/");
	if (pos != std::string::npos)
		dir = dir.substr(0, pos);
	// Release\ → 项目根目录
	dir += "\\..";
	char normalized[MAX_PATH];
	if (GetFullPathNameA(dir.c_str(), MAX_PATH, normalized, nullptr))
		return std::string(normalized);
	return dir;
}

/**
 * @brief 读取文件内容为字符串
 * @param path 文件绝对路径
 * @param out  输出字符串
 * @return true 读取成功
 */
static bool ReadFileToString(const std::string& path, std::string& out)
{
	std::ifstream f(path);
	if (!f.is_open())
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

void ServicePortal::InitJrpcMethods()
{
	if (m_jrpcMethodsInited)
		return;
	m_jrpcMethodsInited = true;

	// ---- 系统 ----

	RegJrpcMethod("ping", "系统",
		"心跳检测，返回 pong",
		"{\"method\":\"ping\",\"params\":{}}",
		"{\"result\":{\"pong\":true}}",
		[](const ZMJSON& /*params*/, ZMJSON& result, ZMJSON& /*error*/) {
			result["pong"] = true;
		});

	RegJrpcMethod("getTime", "系统",
		"获取服务器当前时间",
		"{\"method\":\"getTime\",\"params\":{}}",
		"{\"result\":{\"time\":\"2026-06-09 14:32:05.123\",\"timestamp\":1717835525}}",
		[](const ZMJSON& /*params*/, ZMJSON& result, ZMJSON& /*error*/) {
			time_t now = time(nullptr);
			char buf[32];
			ZmSystem::CurrentTimeStr(buf, sizeof(buf));
			result["time"] = buf;
			result["timestamp"] = (long)now;
		});

	RegJrpcMethod("getStatus", "系统",
		"获取服务器综合状态（HTTP/JRPC/Hub/WS/系统负载/时间）",
		"{\"method\":\"getStatus\",\"params\":{}}",
		"{\"result\":{\"time\":\"...\",\"http\":{\"status\":\"running\",\"port\":80},"
		"\"jrpc_http\":{\"status\":\"running\",\"port\":39440},"
		"\"hub\":{\"status\":\"running\"},\"jrpc_proxy\":{\"status\":\"running\"},"
		"\"websocket\":{\"status\":\"stopped\",\"port\":37310},"
		"\"system\":{\"cpu\":12.5,\"memory\":45.2,\"totalMemMB\":16384,\"usedMemMB\":7400,"
		"\"gpuAvailable\":false,\"gpu\":-1}}}",
		[this](const ZMJSON& /*params*/, ZMJSON& result, ZMJSON& /*error*/) {
			// 时间
			time_t now = time(nullptr);
			char buf[32];
			ZmSystem::CurrentTimeStr(buf, sizeof(buf));
			result["time"] = buf;
			result["timestamp"] = (long)now;

			// HTTP 服务器
			result["http"]["status"] = (m_netDock && m_netDock->IsHttpOpen()) ? "running" : "stopped";
			result["http"]["port"]   = ZM_HTTP_SERVER_PORT;

			// JRPC HTTP
			result["jrpc_http"]["status"] = (m_netDock && m_netDock->IsJrpcHttpOpen()) ? "running" : "stopped";
			result["jrpc_http"]["port"]   = ZM_JSONRPC_SERVER_PORT;

			// Hub
			result["hub"]["status"] = (m_netDock && m_netDock->IsHubOpen()) ? "running" : "stopped";

			// JRPC Proxy
			result["jrpc_proxy"]["status"] = (m_netDock && m_netDock->IsJrpcProxyOpen()) ? "running" : "stopped";

			// WebSocket
			result["websocket"]["status"] = (m_netDock && m_netDock->IsWebSocketOpen()) ? "running" : "stopped";
			result["websocket"]["port"]   = ZM_WS_SERVER_PORT;

			// 系统负载
			auto load = ZmSystem::GetSystemLoad();
			result["system"]["cpu"]           = load.cpu_percent;
			result["system"]["memory"]        = load.memory_percent;
			result["system"]["totalMemMB"]    = load.total_memory_mb;
			result["system"]["usedMemMB"]     = load.used_memory_mb;
			result["system"]["gpuAvailable"]  = load.has_gpu;
			result["system"]["gpu"]           = (load.has_gpu ? load.gpu_percent : -1.0);
		});

	// ---- 测试 ----

	RegJrpcMethod("echo", "测试",
		"通用接口测试，回显传入的数据",
		"{\"method\":\"echo\",\"params\":{\"key\":\"value\"}}",
		"{\"result\":{\"echo\":{\"key\":\"value\"}}}",
		[](const ZMJSON& params, ZMJSON& result, ZMJSON& /*error*/) {
			result["echo"] = params;
		});

	// ---- 文档 ----

	RegJrpcMethod("getRoutes", "文档",
		"获取已注册的 JRPC 方法文档列表",
		"{\"method\":\"getRoutes\",\"params\":{}}",
		"{\"result\":{\"routes\":[{\"method\":\"ping\",\"path\":\"系统\",\"description\":\"...\"}],\"total\":5}}",
		[this](const ZMJSON& /*params*/, ZMJSON& result, ZMJSON& /*error*/) {
			ZMJSON arr = ZMJSON::array();
			for (auto& doc : m_jrpcMethods)
			{
				ZMJSON item;
				item["method"]          = doc.method;
				item["path"]            = doc.path;
				item["description"]     = doc.description;
				item["requestExample"]  = doc.requestExample;
				item["responseExample"] = doc.responseExample;
				arr.push_back(item);
			}
			result["routes"] = arr;
			result["total"]  = (int)arr.size();
		});

	// ---- 关于 ----

	RegJrpcMethod("getAbout", "文档",
		"获取后端和前端的技术信息（README.md）",
		"{\"method\":\"getAbout\",\"params\":{}}",
		"{\"result\":{\"backend\":\"# ZiMoService\\n...\",\"frontend\":\"# 前端\\n...\"}}",
		[this](const ZMJSON& /*params*/, ZMJSON& result, ZMJSON& /*error*/) {
			std::string root = GetProjectRoot();

			// 后端 README.md
			std::string backendMd;
			if (ReadFileToString(root + "\\README.md", backendMd))
				result["backend"] = backendMd;
			else
				result["backend"] = "README.md not found";

			// 前端 README.md
			std::string frontendMd;
			if (ReadFileToString(root + "\\www\\doc\\README.md", frontendMd))
				result["frontend"] = frontendMd;
			else
				result["frontend"] = "www/doc/README.md not found";
		});
}

// ============================================================================
// JRPC 方法分发（JrpcRequestReadCB 和 ProcessInternalJrpc 共用）
// ============================================================================

void ServicePortal::DispatchJrpcMethod(const std::string& method, const ZMJSON& params,
                                        ZMJSON& result, ZMJSON& error)
{
	// 懒初始化方法注册表
	InitJrpcMethods();

	auto it = m_jrpcHandlers.find(method);
	if (it != m_jrpcHandlers.end())
	{
		it->second(params, result, error);
	}
	else
	{
		error["code"]    = -32601;
		error["message"] = "Method not found: " + method;
	}
}

// ============================================================================
// JRPC 请求回调（TAP 链入口）
// ============================================================================

/**
 * @brief JRPC 请求回调（在 ZmThreadPool 线程中执行，由 ZmTapDelegateJRPC 调度）
 *
 * 已在框架层完成线程池投递，本函数在 Worker 线程中运行，无需再手动拷贝数据或 InvokeLater。
 *
 * 操作 TAP 通过 tap->delegate 的异步方法（内部回投到 libevent 线程）：
 *   - delegate->ResponseResultAsync / ResponseErrorAsync
 *   - delegate->SetDropTimerAsync / DropAsync
 * 
 *
 * @note 禁止在此回调中直接调用 Response / SetDropTimer / tap->Drop，
 *       因为当前不在 libevent 线程中执行
 */
void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
{
	ZMJSON result;
	std::string err;
	ZMJSON reqJson = zm_json_parse(reqData, err);

	if (!err.empty())
	{
		ZMJSON errRsp;
		errRsp["code"]    = -32700;
		errRsp["message"] = "Parse error: " + err;
		if (tap->delegate) tap->delegate->ResponseErrorAsync(tap, errRsp);
		return;
	}

	std::string method = zm_json_get_str(reqJson, "method");
	ZMJSON params = reqJson["params"];
	ZMJSON error;

	DispatchJrpcMethod(method, params, result, error);

	if (!error.empty())
		if (tap->delegate) tap->delegate->ResponseErrorAsync(tap, error);
	else
		if (tap->delegate) tap->delegate->ResponseResultAsync(tap, result);
}

// ============================================================================
// 内部 JRPC 请求处理（进程内通道入口，事件循环线程同步调用）
// ============================================================================

/**
 * @brief 处理内部 JRPC 请求并同步返回响应
 *
 * 在事件循环线程中由 ZmNetRequestChannel::Drain 调用。与 JrpcRequestReadCB
 * 共享 DispatchJrpcMethod 分发逻辑，但不经过 TAP 链和异步响应路径。
 *
 * @param requestJson 请求 JSON 字符串（含 method 和 params 字段）
 * @return 完整响应 JSON 字符串（含 result 或 error 字段）
 */
std::string ServicePortal::ProcessInternalJrpc(const std::string& requestJson)
{
	ZMJSON response;

	std::string err;
	ZMJSON reqJson = zm_json_parse(requestJson, err);

	if (!err.empty())
	{
		response["error"]["code"]    = -32700;
		response["error"]["message"] = "Parse error: " + err;
		return response.dump();
	}

	std::string method = zm_json_get_str(reqJson, "method");
	ZMJSON params = reqJson["params"];
	ZMJSON result;
	ZMJSON error;

	DispatchJrpcMethod(method, params, result, error);

	if (!error.empty())
		response["error"] = error;
	else
		response["result"] = result;

	return response.dump();
}
