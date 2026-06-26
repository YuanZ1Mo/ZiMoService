#include "service_portal.h"

#include "service_define.h"
#include "net_dock.h"
#include "http_server_manager.h"
#include "http_server_module_file_hub.h"
#include "broadcast_manager.h"

#include "zm_net_tap.h"
#include "zm_logger.h"
#include "zm_json.h"
#include "zm_util_sys.h"
#include "zm_util_file.h"

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

void ServicePortal::JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData)
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
		ZmTapContext::Response(tap, rsp);
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
		tap->Drop();
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

		rsp_result["hub"]["status"] = (m_netDock && m_netDock->IsHubOpen()) ? "running" : "stopped";

		rsp_result["jrpc_proxy"]["status"] = (m_netDock && m_netDock->IsJrpcProxyOpen()) ? "running" : "stopped";

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
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->ListFiles(path)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "searchFiles")
	{
		std::string keyword = zm_json_get_str(req_params, "keyword", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->SearchFiles(keyword)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "createDir")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string dirName  = zm_json_get_str(req_params, "dirName", "");
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->CreateDir(path, dirName, username, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "deleteItem")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->DeleteItem(path, username, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "verifyDirPassword")
	{
		std::string path     = zm_json_get_str(req_params, "path", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->VerifyDirPassword(path, password)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "changeDirPassword")
	{
		std::string path        = zm_json_get_str(req_params, "path", "");
		std::string username    = zm_json_get_str(req_params, "username", "");
		std::string oldPassword = zm_json_get_str(req_params, "oldPassword", "");
		std::string newPassword = zm_json_get_str(req_params, "newPassword", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->ChangeDirPassword(path, username, oldPassword, newPassword)
			: ZMJSON{{"ok", false}, {"error", "文件中心未初始化"}};
	}
	else if (req_method == "batchDelete")
	{
		ZMJSON paths          = req_params["paths"];
		std::string username = zm_json_get_str(req_params, "username", "");
		std::string password = zm_json_get_str(req_params, "password", "");
		rsp_result = m_netDock->GetHttpServerManager()->GetFileHub() ? m_netDock->GetHttpServerManager()->GetFileHub()->BatchDelete(paths, username, password)
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
	ZmTapContext::Response(tap, rsp);
}
