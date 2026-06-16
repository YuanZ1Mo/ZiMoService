#ifndef HTTP_SERVER_MANAGER_H
#define HTTP_SERVER_MANAGER_H

#include "zm_net_http_router.h"

// 前向声明（头文件中仅通过指针使用）
class ZmHttpServer;
class ZmHttpdTask;
class ZmEvBaseRunLoop;

/**
 * @brief 通用 HTTP 服务器管理器
 *
 * 基于 ZmHttpServer 提供普通 HTTP 服务，不依赖 TAP/Hub/JRPC 代理链。
 * 内部持有独立的 ZmEvBaseRunLoop 事件循环线程，HTTP 请求在事件循环
 * 线程中接收，由 ZmHttpServer 的线程池异步处理。
 *
 * 内部使用 ZmHttpRouter 进行路由分发。采用白名单模式：
 *   所有可访问路径由业务层通过 GetRouter() 显式注册，
 *   未注册路径统一返回 404（不设通配符兜底）。
 */
class HttpServerManager
{
public:
	HttpServerManager();
	~HttpServerManager();

	/**
	 * @brief 启动 HTTP 服务器（内部创建并启动 ZmEvBaseRunLoop）
	 * @param wwwRoot 静态文件根目录绝对路径，为空则不启用静态文件服务
	 * @return true 启动成功
	 */
	bool Open(const char* wwwRoot = nullptr);

	/** @brief 关闭 HTTP 服务器（停事件循环线程、停线程池、释放资源） */
	void Close();

	/**
	 * @brief 获取路由器引用，供业务层注册路由
	 *
	 * 应在 Open() 之后、请求到达之前使用。
	 * 路由器上已安装全局中间件（Logging、Recovery）和静态文件兜底。
	 */
	ZmHttpRouter& GetRouter() { return m_router; }

	/** @brief 查询服务器是否正常运行 */
	bool IsOpen() const;

	/**
	 * @brief 从 wwwRoot 目录读取并返回静态文件（供业务层注册路由时使用）
	 * @param task 请求上下文
	 * @param uri  请求 URI 路径（如 "/control.html"）
	 * @return HTTP 状态码
	 */
	int ServeStaticFile(ZmHttpdTask* task, const std::string& uri);

private:
	/**
	 * @brief HTTP 请求回调，委托给 ZmHttpRouter 分发
	 * @param task 请求上下文
	 * @param data 请求体原始字节
	 * @param dlen 请求体长度
	 * @return HTTP 状态码
	 */
	int OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);

	/**
	 * @brief 根据文件扩展名返回 MIME 类型
	 * @param path 文件路径或 URI
	 * @return MIME 类型字符串，未知类型返回 "application/octet-stream"
	 */
	static const char* GetMimeType(const std::string& path);

	/** @brief 安装全局中间件和静态文件兜底路由（精确路由由业务层通过 GetRouter() 注册） */
	void SetupRouter();

private:
	ZmEvBaseRunLoop* m_evLoop;     ///< 独立事件循环线程（本管理器持有生命周期）
	ZmHttpServer*    m_httpServer; ///< 通用 HTTP 服务器实例
	std::string      m_wwwRoot;    ///< 静态文件根目录绝对路径
	ZmHttpRouter     m_router;     ///< 路由分发器（Express 风格中间件链）
};

#endif // HTTP_SERVER_MANAGER_H
