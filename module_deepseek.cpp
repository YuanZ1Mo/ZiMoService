#include "module_deepseek.h"

#include "zm_net_http_client.h"       // ZmHttpClientRequest/Result/Response/SendAsync
#include "zm_net_http.h"              // ZmHttpdTask(Task()->GetQueryValue/ConnClosedFlag 需完整类型)
#include "zm_net_req_loop.h"
#include "zm_net_req_loop_protocol.h" // ZmReqLoopRest(ResponseJson/ResponseError 静态调用)
#include "zm_logger.h"
#include "zm_util_sys.h"              // ZmSystem::GetModuleDir

#include <cstdio>   // fopen_s/fread/fclose(FILE;项目未开 _CRT_SECURE_NO_WARNINGS,同 zm_net_http_client.cpp 惯例)
#include <ctime>
#include <memory>

// ============================================================================
// 单次查询状态(跨线程:创建于 ZmReqLoop 线程,客户端回调消费,续体回写后释放)
//
// 所有权:shared_ptr 引用计数,持有者 = ① SendAsync 的 params(堆分配 shared_ptr 对象,
// 经投递包 deleter 或关停路径 delete 释放)② close/timeout handler 捕获(loop 释放时销毁)。
// 关停路径回调先行释放 ① 时,② 保证 st 在 handler 最后执行前存活(防 close handler 解引用已释放内存)。
// ============================================================================

struct DeepSeekUsageState
{
	DeepSeekModule*     module;   ///< 关停标志访问
	ZmReqLoop*          loop;     ///< 归属请求线程(投递续体用)
	ZmHttpClientPool*   pool;     ///< 归还客户端
	ZmHttpClient*       client;   ///< 归还池用(续体/放弃路径 Release 后置空)
	ZMJSON*             result;   ///< 组装结果(续体写缓存并回写)
	int64_t*            updatedAt;
	std::string*        errorText;
	uint64_t            qid = 0;  ///< 发起时请求 id(放弃路径 Cancel 用)
};

// 客户端回调(客户端循环线程):组装结果 → 关停检查 → 投递回 ZmReqLoop 线程
// 契约:回调只组装+投递,不得直接回写(task 跨线程);st 释放经 shared_ptr 引用计数
// (params = 堆分配 shared_ptr*,正常路径由投递包 deleter 释放,关停路径直接 delete)
static void DeepSeekUsageCb(ZmHttpClientResult* r, uint64_t /*id*/, void* params)
{
	auto* psp = static_cast<std::shared_ptr<DeepSeekUsageState>*>(params);
	DeepSeekUsageState* st = psp->get();
	if (r && r->Ok())
	{
		const ZmHttpClientResponse& resp = r->Response();
		if (resp.Status() == 200)
		{
			std::string perr;
			(*st->result)["balance"] = zm_json_parse(
				std::string((const char*)resp.Body().data(), resp.Body().size()), perr);
			if (!perr.empty())
			{
				(*st->result)["balance"] = ZMJSON();
				*st->errorText = "DeepSeek response parse failed";
			}
		}
		else if (resp.Status() == 401)
			*st->errorText = "invalid api key (401)";
		else
			*st->errorText = "DeepSeek http " + std::to_string(resp.Status());
	}
	else
	{
		*st->errorText = r ? std::string(r->ErrorText()) : "unknown error";
	}
	*st->updatedAt = (int64_t)time(nullptr);

	if (st->module->IsGone())
	{
		// 关停:不投递续体(loop 可能已销毁);st 由 close/timeout handler 捕获的引用兜底释放
		delete psp;
		return;
	}
	// 投递 ctx = st(续体身份守卫比对用);deleter 释放 params 的 shared_ptr 对象(所有权兜底)
	st->loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_RESPONSE, st,
		[psp](void*) {
			delete psp;
		});
}

// ============================================================================
// DeepSeekModule
// ============================================================================

DeepSeekModule::DeepSeekModule(ZmHttpClientPool* pool)
	: m_pool(pool)
{
	LoadConfig();
}

DeepSeekModule::~DeepSeekModule()
{
	m_gone.store(true);
}

void DeepSeekModule::LoadConfig()
{
	char exePath[MAX_PATH];
	ZmSystem::GetModuleDir(exePath, MAX_PATH);
	std::string cfgPath = std::string(exePath) + "\\..\\config\\deepseek.json";

	// CA bundle:Windows 无 OpenSSL 默认安装时 SSL_CTX_set_default_verify_paths 加载不到任何
	// CA(端到端实测:api.deepseek.com 握手必然失败,报 connect failed)→ 随服务分发
	// Mozilla CA bundle(certs/cacert.pem,随 exe 分发,certs/ 与 exe 同目录)
	m_caFile = std::string(exePath) + "\\certs\\cacert.pem";
	FILE* cf = nullptr;
	errno_t ce = fopen_s(&cf, m_caFile.c_str(), "rb");
	if (ce != 0 || !cf)
	{
		DEFAULT_LOG_WARN("DeepSeek CA bundle 不存在(退化为默认校验路径): {}", m_caFile);
		m_caFile.clear();
	}
	else
		fclose(cf);

	FILE* f = nullptr;
	errno_t fe = fopen_s(&f, cfgPath.c_str(), "rb");
	if (fe != 0 || !f)
	{
		DEFAULT_LOG_WARN("DeepSeek 配置不存在: {}", cfgPath);
		return;
	}
	std::string content;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		content.append(buf, n);
	fclose(f);

	std::string err;
	ZMJSON cfg = zm_json_parse(content, err);
	if (!err.empty())
	{
		DEFAULT_LOG_ERROR("DeepSeek 配置解析失败: {}", err);
		return;
	}
	m_apiKey = zm_json_get_str(cfg, "api_key");
	if (m_apiKey.empty())
		DEFAULT_LOG_WARN("DeepSeek 配置缺少 api_key: {}", cfgPath);
	else
		DEFAULT_LOG_INFO("DeepSeek 配置已加载(api_key 前 6 位: {}...)", m_apiKey.substr(0, 6));
}

bool DeepSeekModule::HandleUsageRequest(ZmReqLoop* loop)
{
	if (m_apiKey.empty() || !m_pool)
	{
		ZmReqLoopRest::ResponseError(loop, 503, "deepseek config not ready");
		return true;
	}

	int64_t now = (int64_t)time(nullptr);
	std::string force = loop->Task()->GetQueryValue("refresh", "");
	bool cacheHit = false;
	ZMJSON out;
	{
		std::lock_guard<std::mutex> lock(m_cacheMutex);   // 缓存读:与续体写互斥(跨 req loop 线程)
		cacheHit = (force != "1" && m_cachedAt > 0 && now - m_cachedAt < CACHE_TTL_SEC);
		if (cacheHit)
		{
			out = m_cache;
			out["updated_at"] = m_cachedAt;
			out["cache_hit"] = true;
		}
	}
	if (cacheHit)
	{
		ZmReqLoopRest::ResponseJson(loop, 200, out);
		return true;
	}

	ZmHttpClient* client = m_pool->Acquire(2000, &loop->Task()->ConnClosedFlag());
	if (!client)
	{
		ZmReqLoopRest::ResponseError(loop, 503, "no client available");
		return true;
	}

	client->SetVerifyMode(true, m_caFile.c_str());   // 客户端级 TLS 校验(CA bundle=certs/cacert.pem;取池后、发请求前设置)
	ZmHttpClientRequest req;
	req.SetUrl("https://api.deepseek.com/user/balance")
	   .SetBearerToken(m_apiKey.c_str())
	   .SetTotalTimeout(10);

	auto stShared = std::make_shared<DeepSeekUsageState>();
	DeepSeekUsageState* st = stShared.get();
	st->module = this;
	st->loop = loop;
	st->pool = m_pool;
	st->client = client;
	st->result = new ZMJSON();
	st->updatedAt = new int64_t(0);
	st->errorText = new std::string();

	// 放弃路径闭环:客户端断开/超时(预算 504 收尾)时取消在飞请求并归还客户端。
	// handler 捕获 shared_ptr:关停路径回调可能先行释放 st,handler 持引用保证本收尾安全解引用。
	auto cancelAndReturn = [stShared](ZmReqLoop*) {
		DeepSeekUsageState* s = stShared.get();
		if (s->client)
		{
			s->client->Cancel(s->qid);      // 取消在飞(若有):回调以 CANCELLED 到达
			s->pool->Release(s->client);    // 归还池(幂等;续体/其它路径判空跳过)
			s->client = nullptr;
		}
	};
	loop->SetCloseHandler(cancelAndReturn);   // CLOSE:ProcessClose 默认收尾(回复门+Release)随后执行
	loop->SetTimeoutHandler([cancelAndReturn](ZmReqLoop* l) {
		cancelAndReturn(l);
		// 注册 onTimeout 后缺省 504 分支不再执行,须自行收尾(仿缺省分支)
		if (l->Task() && l->TryReply())
		{
			l->Task()->SetReply(504, "Request Timeout");
			l->Task()->TriggerReply();
		}
		l->Release();
	});

	loop->SetResponseHandler([this, stShared](ZmReqLoop* l) {
		// 身份守卫:本 RESPONSE post 的 ctx 须等于本请求的状态——放弃请求的陈旧 post
		// 在 loop 被新请求复用时不得运行新请求的续体(归还新请求在飞客户端/污染缓存/错误回 200)
		if (l->GetResponseCtx() != stShared.get())
			return;
		// 续体(ZmReqLoop 线程):归还客户端 → 写缓存 → 回写
		DeepSeekUsageState* s = stShared.get();
		if (s->pool && s->client)
			s->pool->Release(s->client);
		s->client = nullptr;
		if (s->errorText->empty())
		{
			std::lock_guard<std::mutex> lock(m_cacheMutex);   // 缓存写:与命中读互斥(跨 req loop 线程)
			m_cache = *(s->result);
			m_cachedAt = *(s->updatedAt);
		}
		ZMJSON out = *(s->result);
		out["updated_at"] = *(s->updatedAt);
		out["error"] = *(s->errorText);
		ZmReqLoopRest::ResponseJson(l, 200, out);
	});

	uint64_t qid = 0x4000000000000000ULL | (uint64_t)(now & 0x3FFFFFFFFFFFFFFFULL);
	st->qid = qid;
	// params = 堆分配 shared_ptr(所有权转移给回调;正常路径经投递包 deleter 释放,关停路径直接 delete)
	auto* psp = new std::shared_ptr<DeepSeekUsageState>(stShared);
	client->SendAsync(req, qid, psp, DeepSeekUsageCb);
	return true;
}
