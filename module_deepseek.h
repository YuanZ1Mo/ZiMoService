#ifndef MODULE_DEEPSEEK_H
#define MODULE_DEEPSEEK_H

#include "zm_net_http_client_pool.h"
#include "zm_util_json.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class ZmReqLoop;

/**
 * @brief DeepSeek 余额查询业务模块(仿 FileHubModule 模式:构造时自建,析构时自删)
 *
 * 职责:配置加载(config/deepseek.json → api_key)、TTL 缓存、/deepseek/usage 处理逻辑。
 * 通用外呼请求池由 ServicePortal 持有,构造时注入。
 *
 * 线程模型:HandleUsageRequest 在 ZmReqLoop 线程构造请求 → SendAsync(不阻塞) →
 * 客户端回调(客户端循环线程)组装结果 → 关停检查(IsGone)→ PostToLoop(REQ_LOOP_SIG_RESPONSE)
 * 桥回 → 续体(SetResponseHandler)归还客户端、写缓存、回写。
 */
class DeepSeekModule
{
public:
	explicit DeepSeekModule(ZmHttpClientPool* pool);
	~DeepSeekModule();

	/** @brief 处理 GET /deepseek/usage(ZmReqLoop 线程;异步链路,立即返回)
	 *  @return true = 已接管回复;false = 未处理 */
	bool HandleUsageRequest(ZmReqLoop* loop);

	/** @brief 服务停止钩子(ServicePortal::Shutdown 调用):回调不再投递续体(防 UAF) */
	void Shutdown() { m_gone.store(true); }

	/** @brief 关停检查(客户端回调线程调用;经 m_gone 原子标志) */
	bool IsGone() const { return m_gone.load(); }

private:
	void LoadConfig();

	ZmHttpClientPool* m_pool = nullptr;      ///< 通用外呼池(ServicePortal 注入)
	std::string m_apiKey;                    ///< DeepSeek API key(空=未配置)
	std::string m_caFile;                    ///< CA bundle 路径(certs/cacert.pem;空=默认校验路径)
	std::atomic<bool> m_gone{false};         ///< 析构已开始:回调不再投递续体
	ZMJSON  m_cache;                         ///< 余额缓存(进程内 TTL;m_cacheMutex 保护)
	int64_t m_cachedAt = 0;                  ///< 缓存时间戳(秒;m_cacheMutex 保护)
	std::mutex m_cacheMutex;                 ///< 缓存互斥(命中读=HandleUsageRequest,写=续体,跨 req loop 线程)
	static constexpr int64_t CACHE_TTL_SEC = 60;
};

#endif // MODULE_DEEPSEEK_H
