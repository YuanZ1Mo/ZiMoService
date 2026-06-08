#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H
#include "zm_net_tap.h"
#include "zm_net_http_router.h"

#include <vector>
#include <string>

class NetDock;

/**
 * @brief API 路由文档条目
 */
struct RouteDocEntry
{
	std::string method;              ///< HTTP 方法
	std::string path;               ///< 路由路径
	std::string description;        ///< 功能描述
	std::string requestExample;     ///< 请求示例 JSON
	std::string responseExample;    ///< 响应示例 JSON
};

/**
 * @brief JRPC 请求处理门户，接收从 TAP 代理链转发来的 JRPC 请求并响应
 *
 * 同时承载 HTTP API 路由注册（RegisterHttpRoutes）和 JRPC 回调（JrpcRequestReadCB）。
 */
class ServicePortal
{
public:
	ServicePortal() {};
	~ServicePortal() {};

	void SetNetDock(NetDock* nd) { m_netDock = nd; }

	/** @brief 注册 HTTP API 路由 */
	void RegisterHttpRoutes(ZmHttpRouter& router);

public:
	void JrpcRequestReadCB(ZM_TAP_CTX* tap, const char* reqData);

	// --- 同步响应 ---
	void Response(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);
	void ResponseResult(ZM_TAP_CTX* tap, const ZMJSON& jsResult);
	void ResponseError(ZM_TAP_CTX* tap, const ZMJSON& jsError);

	// --- 异步响应 ---
	void ResponseAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResponse);
	void ResponseResultAsync(ZM_TAP_CTX* tap, const ZMJSON& jsResult);
	void ResponseErrorAsync(ZM_TAP_CTX* tap, const ZMJSON& jsError);

private:
	NetDock* m_netDock = nullptr;

	/** @brief 已注册的 API 路由文档列表（供 /api/routes 查询） */
	std::vector<RouteDocEntry> m_routeDocs;

	/** @brief 注册路由并记录文档 */
	void Reg(ZmHttpRouter& router, const char* method, const char* path,
	         ZmHttpRouter::Handler handler, const char* desc,
	         const char* reqExample, const char* rspExample);
};

#endif // SERVICE_PORTAL_H
