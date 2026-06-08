#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"

class NetDock;

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并响应
 *
 * 提供同步和异步两种响应模式：
 * - 同步：直接在 JrpcRequestReadCB 中（libevent 线程）调用 Response/ResponseResult/ResponseError
 * - 异步：在 Worker 线程中处理业务后，调用 ResponseAsync/ResponseResultAsync/ResponseErrorAsync，
 *         内部通过 NetDock::ScheduleTaskInLoop 回投到 libevent 线程安全写入
 *
 * Response/ResponseAsync 接受裸 JSON（调用方自行封装外层），
 * ResponseResult/ResponseError* 自动封装 {"result":...} 或 {"error":...} 外层。
 * 异步超时设置可通过 NetDock::SetDropTimerAsync 在任意线程中安全调用。
 */
class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};

	/**
	 * @brief 设置 NetDock 指针以使用其通用 TAP 操作方法
	 * @param nd NetDock 实例指针，传入 nullptr 清空
	 */
	void SetNetDock(NetDock* nd) { m_netDock = nd; }

public:
	/**
	 * @brief JRPC 请求回调（在 libevent 线程中由 TAP 代理链触发）
	 * @param tap     请求关联的 TAP 上下文
	 * @param reqData 请求 JSON 字符串
	 */
	void JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData);

	// --- 同步响应（必须在 libevent 线程中调用）---

	/** @brief 同步写入裸 JSON 响应（调用方自行封装外层） */
	void Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);

	/** @brief 同步写入 JRPC 成功响应，自动封装 {"result": jsResult} */
	void ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult);

	/** @brief 同步写入 JRPC 错误响应，自动封装 {"error": jsError} */
	void ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError);

	// --- 异步响应（可在任意线程中调用）---

	/** @brief 异步写入裸 JSON 响应（调用方自行封装外层） */
	void ResponseAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);

	/** @brief 异步写入 JRPC 成功响应，自动封装 {"result": jsResult} */
	void ResponseResultAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResult);

	/** @brief 异步写入 JRPC 错误响应，自动封装 {"error": jsError} */
	void ResponseErrorAsync(ZM_TAP_CTX* tap, const ZMJSON& jsError);

private:
	NetDock* m_netDock = nullptr;  ///< NetDock 指针，用于委托通用 TAP 操作
};

#endif // SERVICE_PORTAL_H
