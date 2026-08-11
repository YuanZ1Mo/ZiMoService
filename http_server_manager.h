#ifndef HTTP_SERVER_MANAGER_H
#define HTTP_SERVER_MANAGER_H

#include "zm_net_http_router.h"

class ZmHttpServer;
class ZmHttpdTask;
class ZmEvBaseRunLoop;

/**
 * @brief 通用 HTTP 服务器管理器（网络层）
 *
 * 基于 ZmHttpServer 提供普通 HTTP 服务，内部持有独立的 ZmEvBaseRunLoop。
 * 提供静态文件服务能力。文件中心等业务模块由 ServicePortal 自建自管。
 */
class HttpServerManager
{
public:
	HttpServerManager();
	~HttpServerManager();

	/**
	 * @brief 启动 HTTP/HTTPS 服务器
	 */
	bool Open();
	void Close();

	ZmHttpRouter& GetRouter() { return m_router; }
	bool IsOpen() const;

	/** @brief 热加载 SSL 证书（无需重启服务），HTTP 模式调用无效果 */
	bool ReloadCertificate(const char* certFile, const char* keyFile);

	/** @brief 查询是否已启用 HTTPS（ZmHttpServer 在本头文件仅前向声明，实现在 cpp） */
	bool IsHttps() const;

	/** @brief 设置 TLS session ticket 密钥(转发给内部 ZmHttpServer) */
	void SetTicketKeys(const unsigned char* keys, size_t len);

	/** @brief 投递 ticket 密钥到服务器事件循环线程(轮换场景) */
	void PostSetTicketKeys(const unsigned char* keys, size_t len);
	const std::string& GetWwwRoot() const { return m_wwwRoot; }

	/**
	 * @brief 从 wwwRoot 目录读取并返回静态文件
	 */
	/**
	 * @brief 解析静态资源物理路径(纯查询,无副作用)
	 * @param uri 请求 URI(可带 ?query,自动剥离)
	 * @param outPhysical 命中时输出规范化物理路径
	 * @return true 文件存在且位于 www 根内(含路径穿越防护);false 不存在或非法
	 */
	bool ResolveStaticPath(const std::string& uri, std::string& outPhysical);

	/**
	 * @brief 发送物理文件(零拷贝 evbuffer_file_segment)
	 * @return HTTP 状态码;失败不写任何响应体,返回码对调用方诚实
	 */
	int SendFile(ZmHttpdTask* task, const std::string& physicalPath);

	/**
	 * @brief 静态文件默认策略:解析+发送,文件不存在时输出 404 页面
	 *        需要自定义 404 语义的调用方(如 SPA 回落)请改用 ResolveStaticPath + SendFile
	 */
	int ServeStaticFile(ZmHttpdTask* task, const std::string& uri);

private:
	int  OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);
	void SetupRouter();

private:
	ZmEvBaseRunLoop*     m_evLoop;
	ZmHttpServer*        m_httpServer;
	std::string          m_wwwRoot;
	ZmHttpRouter         m_router;
};

#endif
