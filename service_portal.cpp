#include "service_portal.h"
#include "net_dock.h"
#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_util_thread.h"


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
