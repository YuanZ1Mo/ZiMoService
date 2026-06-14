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
}

// ============================================================================
// 辅助函数
// ============================================================================

static std::string GetProjectRoot()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	std::string dir(exePath);
	size_t pos = dir.find_last_of("\/");
	if (pos != std::string::npos)
		dir = dir.substr(0, pos);
	dir += "\..";
	char normalized[MAX_PATH];
	if (GetFullPathNameA(dir.c_str(), MAX_PATH, normalized, nullptr))
		return std::string(normalized);
	return dir;
}

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

// ============================================================================
// JRPC 请求回调
// ============================================================================

void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
{
	ZMJSON result;
	ZMJSON error;
	std::string err;

	ZMJSON reqJson = zm_json_parse(reqData, err);
	if (!err.empty())
	{
		ZMJSON errRsp;
		errRsp["code"]    = -32700;
		errRsp["message"] = "Parse error: " + err;
		if (tap->delegate)
			tap->delegate->ResponseErrorAsync(tap, errRsp);
		return;
	}

	std::string method = zm_json_get_str(reqJson, "method");
	ZMJSON params = reqJson["params"];

	if (method == "ping")
	{
		result["pong"] = true;
	}
	else if (method == "getTime")
	{
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		result["time"] = buf;
		result["timestamp"] = (long)now;
	}
	else if (method == "getStatus")
	{
		time_t now = time(nullptr);
		char buf[32];
		ZmSystem::CurrentTimeStr(buf, sizeof(buf));
		result["time"] = buf;
		result["timestamp"] = (long)now;

		result["http"]["status"] = (m_netDock && m_netDock->IsHttpOpen()) ? "running" : "stopped";
		result["http"]["port"]   = ZM_HTTP_SERVER_PORT;

		result["jrpc_http"]["status"] = (m_netDock && m_netDock->IsJrpcHttpOpen()) ? "running" : "stopped";
		result["jrpc_http"]["port"]   = ZM_JSONRPC_SERVER_PORT;

		result["hub"]["status"] = (m_netDock && m_netDock->IsHubOpen()) ? "running" : "stopped";

		result["jrpc_proxy"]["status"] = (m_netDock && m_netDock->IsJrpcProxyOpen()) ? "running" : "stopped";

		result["websocket"]["status"] = (m_netDock && m_netDock->IsWebSocketOpen()) ? "running" : "stopped";
		result["websocket"]["port"]   = ZM_WS_SERVER_PORT;

		auto load = ZmSystem::GetSystemLoad();
		result["system"]["cpu"]           = load.cpu_percent;
		result["system"]["memory"]        = load.memory_percent;
		result["system"]["totalMemMB"]    = load.total_memory_mb;
		result["system"]["usedMemMB"]     = load.used_memory_mb;
		result["system"]["gpuAvailable"]  = load.has_gpu;
		result["system"]["gpu"]           = (load.has_gpu ? load.gpu_percent : -1.0);
	}
	else if (method == "echo")
	{
		result["echo"] = params;
	}
	else if (method == "getRoutes")
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
			"{\"method\":\"ping\",\"params\":{}}",
			"{\"result\":{\"pong\":true}}");
		add("getTime",  "系统", "获取服务器当前时间",
			"{\"method\":\"getTime\",\"params\":{}}",
			"{\"result\":{\"time\":\"...\",\"timestamp\":0}}");
		add("getStatus","系统", "获取服务器综合状态",
			"{\"method\":\"getStatus\",\"params\":{}}",
			"{\"result\":{\"http\":{\"status\":\"running\"}}}");
		add("echo",     "测试", "通用接口测试",
			"{\"method\":\"echo\",\"params\":{\"key\":\"value\"}}",
			"{\"result\":{\"echo\":{\"key\":\"value\"}}}");
		add("getRoutes","文档", "获取 JRPC 方法文档列表",
			"{\"method\":\"getRoutes\",\"params\":{}}",
			"{\"result\":{\"routes\":[...],\"total\":6}}");
		add("getAbout", "文档", "获取后端和前端技术信息",
			"{\"method\":\"getAbout\",\"params\":{}}",
			"{\"result\":{\"backend\":\"...\",\"frontend\":\"...\"}}");

		result["routes"] = arr;
		result["total"]  = (int)arr.size();
	}
	else if (method == "getAbout")
	{
		std::string root = GetProjectRoot();

		std::string backendMd;
		if (ReadFileToString(root + "\README.md", backendMd))
			result["backend"] = backendMd;
		else
			result["backend"] = "README.md not found";

		std::string frontendMd;
		if (ReadFileToString(root + "\www\doc\README.md", frontendMd))
			result["frontend"] = frontendMd;
		else
			result["frontend"] = "www/doc/README.md not found";
	}
	else
	{
		error["code"]    = -32601;
		error["message"] = "Method not found: " + method;
	}

	if (tap->delegate)
	{
		if (!error.empty())
			tap->delegate->ResponseErrorAsync(tap, error);
		else
			tap->delegate->ResponseResultAsync(tap, result);
	}
}
