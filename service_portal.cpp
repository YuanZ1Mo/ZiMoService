#include "service_portal.h"
#include "net_dock.h"
#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_util_thread.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "service_define.h"

#include <fstream>
#include <sstream>
#include <ctime>

// ============================================================================
// 同步响应（必须在 libevent 线程中调用）
// ============================================================================

void ServicePortal::Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse)
{
	if (m_netDock)
		m_netDock->Response(tap, jsResponse);
}

void ServicePortal::ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
	ZMJSON rsp;
	rsp["result"] = jsResult;
	Response(tap, rsp);
}

void ServicePortal::ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError)
{
	ZMJSON rsp;
	rsp["error"] = jsError;
	Response(tap, rsp);
}

// ============================================================================
// 异步响应（可在任意线程中调用，内部委托给 NetDock 回投到 libevent 线程）
// ============================================================================

void ServicePortal::ResponseAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResponse)
{
	if (m_netDock)
		m_netDock->ResponseAsync(tap, jsResponse);
}

void ServicePortal::ResponseResultAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResult)
{
	ZMJSON rsp;
	rsp["result"] = jsResult;
	ResponseAsync(tap, rsp);
}

void ServicePortal::ResponseErrorAsync(ZM_TAP_CTX* tap, const ZMJSON& jsError)
{
	ZMJSON rsp;
	rsp["error"] = jsError;
	ResponseAsync(tap, rsp);
}

// ============================================================================
// JRPC 方法分发（JrpcRequestReadCB 和 ProcessInternalJrpc 共用）
// ============================================================================

/**
 * @brief JRPC 方法分发，根据 method 名将结果写入 result 或 error
 * @param method JRPC 方法名
 * @param params JRPC 参数
 * @param result 输出：成功结果（非空表示成功）
 * @param error  输出：错误信息（非空表示失败，含 code 和 message）
 */
void ServicePortal::DispatchJrpcMethod(const std::string& method, const ZMJSON& params,
                                        ZMJSON& result, ZMJSON& error)
{
	if (method == "ping")
	{
		result["pong"] = true;
	}
	else if (method == "getStatus")
	{
		result["status"] = "running";
		result["uptime"] = 12345;
	}
	else
	{
		error["code"] = -32601;
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
 * 操作 TAP 必须统一使用异步版本：
 *   - ResponseResultAsync / ResponseErrorAsync（回投到 libevent 线程写入响应）
 *   - SetDropTimerAsync（回投到 libevent 线程设置超时）
 *   - DropAsync（回投到 libevent 线程释放 TAP）
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
		errRsp["code"] = -32700;
		errRsp["message"] = "Parse error: " + err;
		ResponseErrorAsync(tap, errRsp);
		return;
	}

	std::string method = zm_json_get_str(reqJson, "method");
	ZMJSON params = reqJson["params"];
	ZMJSON error;

	DispatchJrpcMethod(method, params, result, error);

	if (!error.empty())
		ResponseErrorAsync(tap, error);
	else
		ResponseResultAsync(tap, result);
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
		response["error"]["code"] = -32700;
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

// ============================================================================
// 路由文档辅助
// ============================================================================

void ServicePortal::Reg(ZmHttpRouter& router, const char* method, const char* path,
                         ZmHttpRouter::Handler handler, const char* desc,
                         const char* reqExample, const char* rspExample)
{
	if (strcmp(method, "GET") == 0)      router.Get(path, handler);
	else if (strcmp(method, "POST") == 0) router.Post(path, handler);
	else                                  router.Any(path, handler);

	m_routeDocs.push_back({ method, path, desc, reqExample, rspExample });
}

// ============================================================================
// HTTP API 路由注册
// ============================================================================

void ServicePortal::RegisterHttpRoutes(ZmHttpRouter& router)
{
	// ---------- 系统 API ----------

	Reg(router, "GET", "/api/service_time", [](ZmHttpdTask* task, const BYTE*, size_t) {
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));

		ZMJSON rsp;
		rsp["time"] = buf;
		rsp["timestamp"] = (long)now;
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "获取服务器当前时间",
	   "GET /api/service_time",
	   "{\"time\":\"2026-06-08 14:32:05.123\",\"timestamp\":1717835525}");

	Reg(router, "GET", "/api/service_status", [this](ZmHttpdTask* task, const BYTE*, size_t) {
		ZMJSON rsp;

		// HTTP 服务器
		rsp["http"]["status"] = (m_netDock && m_netDock->IsHttpOpen()) ? "running" : "stopped";
		rsp["http"]["port"]   = ZM_HTTP_SERVER_PORT;

		// JRPC HTTP
		rsp["jrpc_http"]["status"] = (m_netDock && m_netDock->IsJrpcHttpOpen()) ? "running" : "stopped";
		rsp["jrpc_http"]["port"]   = ZM_JSONRPC_SERVER_PORT;

		// Hub
		rsp["hub"]["status"] = (m_netDock && m_netDock->IsHubOpen()) ? "running" : "stopped";

		// JRPC Proxy
		rsp["jrpc_proxy"]["status"] = (m_netDock && m_netDock->IsJrpcProxyOpen()) ? "running" : "stopped";

		// WebSocket
		rsp["websocket"]["status"] = (m_netDock && m_netDock->IsWebSocketOpen()) ? "running" : "stopped";
		rsp["websocket"]["port"]   = ZM_WS_SERVER_PORT;

		// 系统负载
		auto load = ZmSystem::GetSystemLoad();
		rsp["system"]["cpu"]        = load.cpu_percent;
		rsp["system"]["memory"]     = load.memory_percent;
		rsp["system"]["totalMemMB"] = load.total_memory_mb;
		rsp["system"]["usedMemMB"]  = load.used_memory_mb;
		rsp["system"]["gpuAvailable"] = load.has_gpu;
		rsp["system"]["gpu"]        = (load.has_gpu ? load.gpu_percent : -1.0);

		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "获取服务器综合状态（HTTP/JRPC/Hub/WS/系统负载）",
	   "GET /api/service_status",
	   "{\"http\":{\"status\":\"running\",\"port\":80},\"jrpc_http\":{\"status\":\"running\",\"port\":39440},"
	   "\"hub\":{\"status\":\"running\"},\"jrpc_proxy\":{\"status\":\"running\"},"
	   "\"websocket\":{\"status\":\"stopped\",\"port\":37310},"
	   "\"system\":{\"cpu\":12.5,\"memory\":45.2,\"totalMemMB\":16384,\"usedMemMB\":7400,\"gpuAvailable\":false,\"gpu\":-1}}");

	// ---------- 接口测试 ----------

	Reg(router, "ANY", "/api/api_test", [](ZmHttpdTask* task, const BYTE* data, size_t dlen) {
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		if (task->Method() != EVHTTP_REQ_POST)
		{
			std::string body = "{\"message\":\"请使用 POST 方法发送 JSON 数据\"}";
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
			return 200;
		}
		if (!data || dlen == 0)
		{
			std::string body = "{\"message\":\"请求体为空，请输入 JSON\"}";
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
			return 200;
		}
		std::string reqBody((const char*)data, dlen);
		std::string err;
		ZMJSON reqJson = zm_json_parse(reqBody, err);
		ZMJSON rsp;
		rsp["echo"] = err.empty() ? reqJson : ZMJSON(reqBody);
		rsp["length"] = (int)dlen;
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "通用接口测试，POST JSON 并回显",
	   "POST /api/api_test\nContent-Type: application/json\n\n{\"key\":\"value\"}",
	   "{\"echo\":{\"key\":\"value\"},\"length\":16}");

	// ---------- 路由文档 ----------

	Reg(router, "GET", "/api/routes", [this](ZmHttpdTask* task, const BYTE*, size_t) {
		ZMJSON rsp;
		ZMJSON arr = ZMJSON::array();
		for (auto& doc : m_routeDocs)
		{
			ZMJSON item;
			item["method"]          = doc.method;
			item["path"]            = doc.path;
			item["description"]     = doc.description;
			item["requestExample"]  = doc.requestExample;
			item["responseExample"] = doc.responseExample;
			arr.push_back(item);
		}
		rsp["routes"] = arr;
		rsp["total"] = (int)arr.size();
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "获取已注册的 API 路由文档列表",
	   "GET /api/routes",
	   "{\"routes\":[{\"method\":\"GET\",\"path\":\"/api/service_time\",...}],\"total\":6}");

	// ---------- 关于（读取 README 文件）----------

	Reg(router, "GET", "/api/about/backend", [](ZmHttpdTask* task, const BYTE*, size_t) {
		// 从 exe 目录上翻一层找 README.md
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string dir(exePath);
		size_t pos = dir.find_last_of("\\/");
		if (pos != std::string::npos) dir = dir.substr(0, pos);
		std::string readmePath = dir + "\\..\\README.md";

		std::ifstream f(readmePath);
		if (!f.is_open())
		{
			task->PutReplyHeader("Content-type", "text/plain; charset=utf-8");
			std::string body = "README.md not found at: " + readmePath;
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
			return 404;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		std::string body = ss.str();
		task->PutReplyHeader("Content-type", "text/markdown; charset=utf-8");
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "获取后端 README.md（架构和技术栈）",
	   "GET /api/about/backend",
	   "# ZiMoService\n\nZiMo 客户端生态的核心 Windows 服务...");

	Reg(router, "GET", "/api/about/frontend", [](ZmHttpdTask* task, const BYTE*, size_t) {
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string dir(exePath);
		size_t pos = dir.find_last_of("\\/");
		if (pos != std::string::npos) dir = dir.substr(0, pos);
		std::string readmePath = dir + "\\..\\www\\README.md";

		std::ifstream f(readmePath);
		if (!f.is_open())
		{
			task->PutReplyHeader("Content-type", "text/plain; charset=utf-8");
			std::string body = "www/README.md not found";
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
			return 404;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		std::string body = ss.str();
		task->PutReplyHeader("Content-type", "text/markdown; charset=utf-8");
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	}, "获取前端 README.md（前端技术信息）",
	   "GET /api/about/frontend",
	   "# 前端\n\n基于 Vue 3 + ZmHttpRouter...");
}
