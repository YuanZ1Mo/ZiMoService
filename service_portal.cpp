#include "service_portal.h"
#include "net_dock.h"
#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_util_thread.h"
#include "zm_json.h"
#include "zm_util_sys.h"


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
// JRPC 请求回调
// ============================================================================

/**
 * @brief JRPC 请求回调（在 libevent 线程中由 TAP 代理链触发）
 *
 * 提供两种处理模式示例：
 * 1. 同步模式（已注释）：直接在当前线程响应，适用于无需耗时的简单业务
 * 2. 异步模式（当前启用）：投递到线程池处理，通过 ResponseResultAsync/ResponseErrorAsync 回写
 *
 * ZM_TAP_STATE_INUSE
 * ZM_TAP_CTX
 * ZmThreadPool
 */
void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
{
	//// =========================================================================
	//// 同步响应模式示例
	//// =========================================================================
	//ResponseResult(tap, { { "isOk", 1 } });

	// =========================================================================
	// 异步响应模式
	// =========================================================================

	 // 1. 拷贝请求数据（tap->requester_data 在回调返回后可能被覆盖）
	 std::string reqCopy(reqData);

	 // 2. 投递到线程池处理业务
	 ZmThreadPool::InvokeLater([this, tap, reqCopy](void*) {
		 // ===== Worker 线程 =====

		 ZMJSON result;
		 std::string err;
		 ZMJSON reqJson = zm_json_parse(reqCopy, err);

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
			 ZMJSON errRsp;
			 errRsp["code"] = -32601;
			 errRsp["message"] = "Method not found: " + method;
			 ResponseErrorAsync(tap, errRsp);
			 return;
		 }

		 ResponseResultAsync(tap, result);

	 }, nullptr, 0);
}

// ============================================================================
// HTTP API 路由注册
// ============================================================================

void ServicePortal::RegisterHttpRoutes(ZmHttpRouter& router)
{
	// GET /api/status — 服务状态
	router.Get("/api/status", [](ZmHttpdTask* task, const BYTE*, size_t) {
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));

		ZMJSON rsp;
		rsp["server"] = "ZiMoService";
		rsp["version"] = "1.0";
		rsp["time"] = buf;
		rsp["timestamp"] = (long)now;
#ifdef _WIN64
		rsp["arch"] = "x64";
#else
		rsp["arch"] = "x86";
#endif
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	});

	// GET /api/items — 数据列表
	router.Get("/api/items", [](ZmHttpdTask* task, const BYTE*, size_t) {
		ZMJSON rsp;
		rsp["items"] = {
			{ {"id", 1}, {"name", "项目 Alpha"},   {"status", "运行中"} },
			{ {"id", 2}, {"name", "项目 Beta"},    {"status", "已停止"} },
			{ {"id", 3}, {"name", "项目 Gamma"},   {"status", "运行中"} },
			{ {"id", 4}, {"name", "项目 Delta"},   {"status", "维护中"} },
			{ {"id", 5}, {"name", "项目 Epsilon"}, {"status", "运行中"} }
		};
		rsp["total"] = 5;
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	});

	// ANY /api/echo — 回显测试
	router.Any("/api/echo", [](ZmHttpdTask* task, const BYTE* data, size_t dlen) {
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");

		if (task->Method() == EVHTTP_REQ_POST && data && dlen > 0)
		{
			std::string reqBody((const char*)data, dlen);
			std::string err;
			ZMJSON reqJson = zm_json_parse(reqBody, err);

			ZMJSON rsp;
			rsp["echo"] = err.empty() ? reqJson : ZMJSON(reqBody);
			rsp["length"] = (int)dlen;
			std::string body = rsp.dump();
			task->SetReplyData((const BYTE*)body.c_str(), body.size());
			return 200;
		}

		ZMJSON rsp;
		rsp["message"] = (task->Method() == EVHTTP_REQ_POST)
			? "请求体为空，试试 POST 一段 JSON"
			: "请使用 POST 方法发送 JSON 数据";
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	});

	// GET /api/time — 服务器时间
	router.Get("/api/time", [](ZmHttpdTask* task, const BYTE*, size_t) {
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
	});

	// GET /api/users/:id — 路径参数示例
	router.Get("/api/users/:id", [](ZmHttpdTask* task, const BYTE*, size_t) {
		std::string uid = ZmHttpRouter::GetParam("id");
		ZMJSON rsp;
		rsp["userId"] = uid;
		rsp["message"] = "路径参数示例";
		task->PutReplyHeader("Content-type", "application/json; charset=utf-8");
		std::string body = rsp.dump();
		task->SetReplyData((const BYTE*)body.c_str(), body.size());
		return 200;
	});
}
