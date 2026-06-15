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
}

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 获取项目根目录（exe 所在目录的上一级）
 * @return 规范化后的绝对路径，失败时返回空字符串
 */
static std::string GetProjectRoot()
{
	char exePath[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
	{
		DEFAULT_LOG_ERROR("GetProjectRoot: GetModuleFileNameA 失败，len={}", len);
		return "";
	}

	std::string dir(exePath);
	size_t pos = dir.find_last_of("\\/");
	if (pos != std::string::npos)
		dir = dir.substr(0, pos);

	// 从 $(Configuration)\ 目录上翻一级到项目根目录
	dir += "\\..";
	char normalized[MAX_PATH];
	DWORD normLen = GetFullPathNameA(dir.c_str(), MAX_PATH, normalized, nullptr);
	if (normLen == 0 || normLen >= MAX_PATH)
	{
		DEFAULT_LOG_ERROR("GetProjectRoot: GetFullPathNameA 失败，dir={}, err={}",
			dir, ZmSystem::ErrMsg(-1));
		return dir;  // 回退到未规范化的路径
	}

	std::string result(normalized);
	// 去掉可能存在的尾部反斜杠
	while (!result.empty() && (result.back() == '\\' || result.back() == '/'))
		result.pop_back();

	DEFAULT_LOG_INFO("GetProjectRoot: exePath={}, root={}", exePath, result);
	return result;
}

/**
 * @brief 读取文件内容到字符串（二进制模式，保留原始字节）
 * @param path 文件绝对路径
 * @param out  输出字符串
 * @return true 读取成功
 */
static bool ReadFileToString(const std::string& path, std::string& out)
{
	// 使用二进制模式避免 Windows 文本模式的 \r\n 转换和 \x1A EOF 截断
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open())
	{
		DEFAULT_LOG_ERROR("ReadFileToString: 无法打开文件 path={}, err={}",
			path, ZmSystem::ErrMsg(-1));
		return false;
	}

	std::stringstream ss;
	ss << f.rdbuf();
	out = ss.str();

	DEFAULT_LOG_INFO("ReadFileToString: 读取成功 path={}, size={}", path, out.size());
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
		ZMJSON rsp;
		rsp["error"]["code"]    = -32700;
		rsp["error"]["message"] = "Parse error: " + err;
		ZmTapContext::Response(tap, rsp);
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

		result["broadcast"]["status"]      = (m_netDock && m_netDock->IsBroadcastOpen()) ? "running" : "stopped";
		result["broadcast"]["port"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetPort() : 0;
		result["broadcast"]["connections"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetConnectionCount() : 0;
		result["broadcast"]["sent"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetSentCount() : 0;
		auto load = ZmSystem::GetSystemLoad();
		result["system"]["cpu"]           = load.cpu_percent;
		result["system"]["memory"]        = load.memory_percent;
		result["system"]["totalMemMB"]    = load.total_memory_mb;
		result["system"]["usedMemMB"]     = load.used_memory_mb;
		result["system"]["gpuAvailable"]  = load.has_gpu;
		result["system"]["gpu"]           = (load.has_gpu ? load.gpu_percent : -1.0);
	}
		else if (method == "broadcast")
		{
			std::string topic   = zm_json_get_str(params, "topic", "");
			std::string content = zm_json_get_str(params, "content", "");
			std::string tag     = zm_json_get_str(params, "tag", "");

			if (topic.empty())
			{
				result["success"] = false;
				result["error"]   = "topic is required";
			}
			else
			{
				result["success"] = BroadcastMessage(topic, content, tag);
			}
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
		add("broadcast","广播", "向所有匹配tag的客户端广播消息",
			"{\"method\":\"broadcast\",\"params\":{\"topic\":\"alert\",\"content\":\"{\\\"msg\\\":\\\"hello\\\"}\",\"tag\":\"all\"}}",
			"{\"result\":{\"success\":true}}");
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
		if (root.empty())
		{
			error["code"]    = -32603;
			error["message"] = "无法获取项目根目录";
		}
		else
		{
			std::string backendMd;
			if (ReadFileToString(root + "\\README.md", backendMd))
				result["backend"] = backendMd;
			else
				result["backend"] = "README.md not found (path: " + root + "\\README.md)";

			std::string frontendMd;
			if (ReadFileToString(root + "\\www\\doc\\README.md", frontendMd))
				result["frontend"] = frontendMd;
			else
				result["frontend"] = "www/doc/README.md not found (path: " + root + "\\www\\doc\\README.md)";
		}
	}
	else
	{
		error["code"]    = -32601;
		error["message"] = "Method not found: " + method;
	}

	ZMJSON rsp;
	if (!error.empty())
		rsp["error"] = error;
	else
		rsp["result"] = result;
	ZmTapContext::Response(tap, rsp);
}
