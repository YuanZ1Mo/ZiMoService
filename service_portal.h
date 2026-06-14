#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

class NetDock;
class HttpServerManager;

/**
 * @brief JRPC 方法文档条目
 */
struct RouteDocEntry
{
	std::string method;              ///< JRPC 方法名
	std::string path;               ///< 分类标签（如 "系统"、"测试"）
	std::string description;        ///< 功能描述
	std::string requestExample;     ///< 请求示例 JSON
	std::string responseExample;    ///< 响应示例 JSON
};

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并响应
 *
 * 所有业务逻辑统一通过 JRPC 方法（端口 39440）对外暴露。
 * 内部 HTTP JRPC 请求和外部 TAP 连接走同一条 Hub 代理链，共享方法分发逻辑。
 */
class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};

	void SetNetDock(NetDock* nd) { m_netDock = nd; }

	/**
	 * @brief 注册 HTTP 80 端口路由（供 ServiceCenter 在启动时调用）
	 * @param httpMgr HTTP 服务器管理器，提供 ServeStaticFile 能力
	 */
	void RegisterHttpRoutes(HttpServerManager* httpMgr);

public:
	/** @brief TAP 链入口：JRPC 请求回调（在 JRPC delegate 线程池中执行） */
	void JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData);

private:
	/** @brief JRPC 方法处理器签名：参数 → 结果/错误 */
	using JrpcHandler = std::function<void(const ZMJSON& params, ZMJSON& result, ZMJSON& error)>;

	/**
	 * @brief JRPC 方法分发（JrpcRequestReadCB 和 ProcessInternalJrpc 共用）
	 * @param method JRPC 方法名
	 * @param params JRPC 参数
	 * @param result 输出：成功结果（非空表示成功）
	 * @param error  输出：错误信息（非空表示失败）
	 */
	void DispatchJrpcMethod(const std::string& method, const ZMJSON& params,
	                        ZMJSON& result, ZMJSON& error);

	/**
	 * @brief 初始化 JRPC 方法注册表（构造时调用一次）
	 *
	 * 将方法名映射到处理函数，并记录文档条目到 m_jrpcMethods。
	 */
	void InitJrpcMethods();

	/**
	 * @brief 注册一个 JRPC 方法及其文档
	 * @param name            方法名
	 * @param category        分类标签
	 * @param description     功能描述
	 * @param requestExample  请求示例 JSON
	 * @param responseExample 响应示例 JSON
	 * @param handler         处理函数
	 */
	void RegJrpcMethod(const char* name, const char* category,
	                   const char* description,
	                   const char* requestExample, const char* responseExample,
	                   JrpcHandler handler);

	NetDock* m_netDock = nullptr;

	/** @brief JRPC 方法处理器映射表（method → handler） */
	std::unordered_map<std::string, JrpcHandler> m_jrpcHandlers;

	/** @brief 已注册的 JRPC 方法文档列表（供 getRoutes 查询） */
	std::vector<RouteDocEntry> m_jrpcMethods;

	/** @brief 标记方法表是否已初始化 */
	bool m_jrpcMethodsInited = false;
};

#endif // SERVICE_PORTAL_H
