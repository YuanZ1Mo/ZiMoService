#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"
#include <functional>

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并响应
 *
 * 提供同步和异步两种响应模式：
 * - 同步：直接在 JrpcRequestReadCB 回调中调用 ResponseResult/ResponseError
 * - 异步：在 Worker 线程中处理业务后，调用 ResponseResultAsync/ResponseErrorAsync，
 *         内部通过 ScheduleFn 将响应回投到 libevent 线程安全写入
 */
class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};

	/**
	 * @brief 事件循环线程调度回调类型，签名与 NetDock::ScheduleTaskInLoop 一致
	 * @param fn_run 在 libevent 线程中执行的任务
	 * @param fn_cb  任务执行后的回调（可选）
	 * @param param  透传参数（可选）
	 * @return true 调度成功，false 事件循环未就绪
	 */
	using ScheduleFn = std::function<bool(
		std::function<void(void*)>,
		std::function<void(void*)>,
		void*)>;

	/**
	 * @brief 设置事件循环线程调度回调（需在 JrpcRequestReadCB 触发之前调用）
	 * @param fn 调度函数，传入 nullptr 清空
	 */
	void SetScheduleFn(ScheduleFn fn) { m_scheduleFn = fn; }

public:
	/**
	 * @brief JRPC 请求回调（在 libevent 线程中由 TAP 代理链触发）
	 * @param tap     请求关联的 TAP 上下文
	 * @param reqData 请求 JSON 字符串
	 *
	 * 可在 libevent 线程中同步调用 ResponseResult/ResponseError 直接响应，
	 * 也可将业务逻辑投递到 Worker 线程处理，完成后通过 ResponseResultAsync 回写响应
	 */
	void JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData);

	/**
	 * @brief 同步写入 JRPC 成功响应（必须在 libevent 线程中调用）
	 * @param tap      请求关联的 TAP 上下文
	 * @param jsResult 响应 result 对象
	 */
	void ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult);

	/**
	 * @brief 同步写入 JRPC 错误响应（必须在 libevent 线程中调用）
	 * @param tap     请求关联的 TAP 上下文
	 * @param jsError 响应 error 对象
	 */
	void ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError);

	/**
	 * @brief 异步写入 JRPC 成功响应（可在任意线程中调用）
	 * @param tap      请求关联的 TAP 上下文
	 * @param jsResult 响应 result 对象
	 *
	 * 内部通过 ScheduleFn 将实际写入回投到 libevent 线程，
	 * 回投时会检查 TAP 状态（已 Drop 则丢弃响应并输出警告日志）
	 */
	void ResponseResultAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResult);

	/**
	 * @brief 异步写入 JRPC 错误响应（可在任意线程中调用）
	 * @param tap     请求关联的 TAP 上下文
	 * @param jsError 响应 error 对象
	 */
	void ResponseErrorAsync(ZM_TAP_CTX* tap, const ZMJSON& jsError);

private:
	/**
	 * @brief 内部响应写入（必须在 libevent 线程中调用）
	 *
	 * 从 TAP 回传链上弹出 JRPC delegate，设置回传数据后触发 OnTapDelegateBackEvent
	 * 将响应通过长度前缀帧写回客户端
	 */
	void Response(ZM_TAP_CTX* tap, const ZMJSON& jsResult);

	ScheduleFn m_scheduleFn;  ///< 事件循环线程调度回调，用于异步响应回投
};


#endif // SERVICE_PORTAL_H