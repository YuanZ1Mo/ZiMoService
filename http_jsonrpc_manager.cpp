#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

HttpJsonRpcManager::HttpJsonRpcManager()
	: m_evbase(nullptr)
	, m_httpServerJRPC(nullptr)
	, m_hubMgr(nullptr)
	, m_channel(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
	Close();
}

bool HttpJsonRpcManager::Open(struct event_base* evbase, HubProxyManager* hubMgr)
{
	if (!evbase || !hubMgr)
		return false;

	m_evbase = evbase;
	m_hubMgr = hubMgr;

	// 1. 创建内部 JRPC 请求通道（必须在 HTTP 服务器启动前，确保 Worker 线程可用）
	if (!OpenJrpcChannel(hubMgr))
	{
		DEFAULT_LOG_ERROR("OpenJrpcChannel failed");
		return false;
	}

	// 2. 创建 HTTP JSON-RPC 服务器
	if (nullptr == m_httpServerJRPC)
	{
		m_httpServerJRPC = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT);
		m_httpServerJRPC->Start();
		m_httpServerJRPC->SetJsonRpcCBEx(std::bind(&HttpJsonRpcManager::OnHttpJsonrpcEx, this,
			std::placeholders::_1, std::placeholders::_2,
			std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
	}

	return (nullptr != m_httpServerJRPC);
}

void HttpJsonRpcManager::Close()
{
	// ★ 先关通道（拒绝所有 pending promise → Worker 线程唤醒），再停 HTTP 服务器（join 线程池）
	CloseJrpcChannel();

	if (m_httpServerJRPC)
	{
		m_httpServerJRPC->Stop();
		delete m_httpServerJRPC;
		m_httpServerJRPC = nullptr;
	}

	m_hubMgr = nullptr;
	m_evbase = nullptr;
}

// ============================================================================
// 内部 JRPC 请求通道
// ============================================================================

bool HttpJsonRpcManager::OpenJrpcChannel(HubProxyManager* hubMgr)
{
	if (m_channel)
	{
		DEFAULT_LOG_WARN("ZmNetRequestChannel already opened, skipping");
		return true;
	}

	if (!m_evbase)
	{
		DEFAULT_LOG_ERROR("OpenJrpcChannel failed: evbase not set");
		return false;
	}

	if (!hubMgr || !hubMgr->HubProxy())
	{
		DEFAULT_LOG_ERROR("OpenJrpcChannel failed: Hub not available");
		return false;
	}

	m_channel = new ZmNetRequestChannel();
	if (!m_channel->Open(m_evbase,
			std::bind(&HttpJsonRpcManager::InjectJrpcRequest, this,
				std::placeholders::_1, std::placeholders::_2)))
	{
		DEFAULT_LOG_ERROR("OpenJrpcChannel failed: channel Open() returned false");
		delete m_channel;
		m_channel = nullptr;
		return false;
	}

	DEFAULT_LOG_INFO("JrpcChannel opened");
	return true;
}

void HttpJsonRpcManager::CloseJrpcChannel()
{
	if (!m_channel)
		return;

	m_channel->Close(m_evbase);
	delete m_channel;
	m_channel = nullptr;

	DEFAULT_LOG_INFO("JrpcChannel closed");
}

/**
 * @brief 将内部 JRPC 请求通过 bufferevent_pair 注入 Hub 代理链
 *
 * 数据路径：
 *   ① bufferevent_pair_new 创建内存级互联 pair（零内核穿越）
 *   ② pair[0] 写入 "JRPC" + 4字节大端长度 + JSON（与原 socketpair 帧格式一致）
 *   ③ pair[1] 通过 OnPairAcceptBev 注入 Hub（协议探测 → JRPC delegate → ServicePortal）
 *   ④ Hub 处理完毕后 WriteResponse 写入 pair[1] → 数据到达 pair[0]
 *   ⑤ pair[0] 读回调拼包 → 触发 callback → promise.set_value → Worker 线程唤醒
 *
 * pair[1] 生命周期由 TAP 管理（BEV_OPT_CLOSE_ON_FREE，Drop 时释放）。
 * pair[0] 在响应读取完毕或出错时释放。
 */
void HttpJsonRpcManager::InjectJrpcRequest(const std::string& request_json,
                                            std::function<void(std::string)> callback)
{
	// 1. 创建 bufferevent pair（内存级互联，零内核穿越）
	struct bufferevent* pair[2] = { nullptr, nullptr };
	if (bufferevent_pair_new(m_evbase, ZM_EVENT_BEV_OPTIONS, pair) < 0)
	{
		DEFAULT_LOG_ERROR("InjectJrpcRequest: bufferevent_pair_new failed");
		callback(std::string());
		return;
	}

	// 2. 写入 JRPC 请求帧到 pair[0]：4字节魔术 "JRPC" + 4字节大端长度 + JSON 体
	char head[8];
	memcpy(head, "JRPC", 4);
	uint32_t qlen = htonl((uint32_t)request_json.size());
	memcpy(head + 4, &qlen, 4);

	struct evbuffer* output = bufferevent_get_output(pair[0]);
	if (evbuffer_add(output, head, 8) < 0 ||
		evbuffer_add(output, request_json.data(), request_json.size()) < 0)
	{
		DEFAULT_LOG_ERROR("InjectJrpcRequest: evbuffer_add failed");
		bufferevent_free(pair[0]);
		bufferevent_free(pair[1]);
		callback(std::string());
		return;
	}

	// 3. 将 pair[1] 注入 Hub 代理链
	if (!ZmTapContextEventHandler::OnPairAcceptBev(m_hubMgr->HubProxy(), pair[1]))
	{
		DEFAULT_LOG_ERROR("InjectJrpcRequest: OnPairAcceptBev failed");
		bufferevent_free(pair[0]);
		// pair[1] 已由 OnPairAcceptBev 在失败路径中释放
		callback(std::string());
		return;
	}
	// pair[1] 所有权已转移给 TAP，TAP Drop 时自动释放（BEV_OPT_CLOSE_ON_FREE）

	// 4. 在 pair[0] 上设置读回调，等待 Hub 回写响应
	auto* rctx = new ResponseReadCtx{ std::move(callback), pair[0], 0, false, {} };
	bufferevent_setcb(pair[0], HttpJsonRpcManager::OnResponseRead, nullptr,
	                   HttpJsonRpcManager::OnResponseEvent, rctx);
	bufferevent_setwatermark(pair[0], EV_READ, 4, 0); // 至少 4 字节（长度头）
	bufferevent_enable(pair[0], EV_READ);
}

// ============================================================================
// bufferevent_pair 响应读取回调
// ============================================================================

void HttpJsonRpcManager::OnResponseRead(struct bufferevent* bev, void* ctx)
{
	auto* rctx = static_cast<ResponseReadCtx*>(ctx);
	struct evbuffer* input = bufferevent_get_input(bev);

	// 第一阶段：读取 4 字节长度头
	if (!rctx->header_read)
	{
		if (evbuffer_get_length(input) < 4)
			return; // 数据不足，继续等待

		uint32_t len;
		evbuffer_remove(input, &len, 4);
		rctx->response_len = ntohl(len);
		rctx->header_read = true;
	}

	// 第二阶段：读取响应 JSON 体
	size_t available = evbuffer_get_length(input);
	if (rctx->header_read && available >= rctx->response_len)
	{
		rctx->buffer.resize(rctx->response_len);
		evbuffer_remove(input, &rctx->buffer[0], rctx->response_len);

		// 通知 Worker 线程
		rctx->callback(std::move(rctx->buffer));

		// 清理：释放 pair[0] 和上下文
		bufferevent_free(rctx->pair0);
		delete rctx;
	}
}

void HttpJsonRpcManager::OnResponseEvent(struct bufferevent* bev, short events, void* ctx)
{
	auto* rctx = static_cast<ResponseReadCtx*>(ctx);

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
	{
		DEFAULT_LOG_WARN("JRPC response channel closed before receiving full response, events={}", events);
		rctx->callback(std::string()); // 空字符串 → Worker 端返回 false
	}

	bufferevent_free(rctx->pair0);
	delete rctx;
}

// ============================================================================
// HTTP 回调
// ============================================================================

bool HttpJsonRpcManager::SendToHubProxy(const char* reqjs, std::string& rspjs)
{
	if (!m_channel)
	{
		DEFAULT_LOG_ERROR("ZmNetRequestChannel not set, cannot deliver request");
		return false;
	}

	std::future<std::string> future = m_channel->Submit(reqjs);

	auto status = future.wait_for(std::chrono::seconds(30));
	if (status != std::future_status::ready)
	{
		DEFAULT_LOG_ERROR("Wait for JRPC response timeout or channel closed");
		return false;
	}

	rspjs = future.get();
	return !rspjs.empty();
}

int HttpJsonRpcManager::OnHttpJsonrpcEx(ZmHttpdTask* task, const std::string& method,
                                        const ZMJSON& params, ZMJSON& result, ZMJSON& error)
{
	int code = 0;

	ZMJSON reqobj;
	reqobj["method"] = method;
	reqobj["ip"] = task->Ip();
	reqobj["port"] = task->Port();
	reqobj["request_useragent"] = task->GetRequestHeader("User-Agent");
	reqobj["params"] = params;

	std::string repjstr;
	if (!SendToHubProxy(reqobj.dump().c_str(), repjstr))
	{
		error["code"] = ZM_JRPC_ERR_SEND_HUB;
		error["message"] = "SendToHubProxy error";
		return ZM_JRPC_ERR_SEND_HUB;
	}

	if (repjstr.empty())
	{
		error["code"] = ZM_JRPC_ERR_EMPTY_RSP;
		error["message"] = "Response is empty";
		return ZM_JRPC_ERR_EMPTY_RSP;
	}

	std::string jerrstr;
	ZMJSON repjson = zm_json_parse(repjstr, jerrstr);
	if (!repjson.is_object())
	{
		error["code"] = ZM_JRPC_ERR_FORMAT;
		error["message"] = "Response format error";
		return ZM_JRPC_ERR_FORMAT;
	}

	if (repjson["result"].is_object())
		result = repjson["result"];
	if (repjson["error"].is_object())
		error = repjson["error"];

	return code;
}
