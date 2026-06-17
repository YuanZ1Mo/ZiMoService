#ifndef HTTP_SERVER_MANAGER_H
#define HTTP_SERVER_MANAGER_H

#include "zm_net_http_router.h"

class ZmHttpServer;
class ZmHttpdTask;
class ZmEvBaseRunLoop;
class HttpServerModuleFileHub;

/**
 * @brief 通用 HTTP 服务器管理器
 *
 * 基于 ZmHttpServer 提供普通 HTTP 服务，内部持有独立的 ZmEvBaseRunLoop。
 * 提供静态文件服务、通用文件下载和上传能力。
 * 内部创建并管理 HttpServerModuleFileHub 的生命周期。
 */
class HttpServerManager
{
public:
	HttpServerManager();
	~HttpServerManager();

	bool Open(const char* wwwRoot = nullptr);
	void Close();

	ZmHttpRouter& GetRouter() { return m_router; }
	bool IsOpen() const;

	/** @brief 获取文件中心模块指针（供 ServicePortal 调用 JRPC 方法） */
	HttpServerModuleFileHub* GetFileHub() { return m_fileHub; }

	/**
	 * @brief 从 wwwRoot 目录读取并返回静态文件
	 */
	int ServeStaticFile(ZmHttpdTask* task, const std::string& uri);

	/**
	 * @brief 通用文件下载：将物理路径指向的文件以 Content-Disposition: attachment 发送
	 * @param task         请求上下文
	 * @param physicalPath 文件绝对路径
	 * @return HTTP 状态码（200 成功，206 Range 部分内容，404/500 失败）
	 */
	int SendFile(ZmHttpdTask* task, const std::string& physicalPath);

	/**
	 * @brief 通用文件上传：将请求体写入指定物理路径（零拷贝 mmap）
	 * @param task         请求上下文
	 * @param physicalPath 目标文件绝对路径（父目录不存在时自动创建）
	 * @param data         请求体数据指针
	 * @param dlen         请求体长度
	 * @return HTTP 状态码（201 成功，400/500 失败）
	 */
	int ReceiveFile(ZmHttpdTask* task, const std::string& physicalPath,
		const BYTE* data, size_t dlen);

private:
	int  OnHttpRequest(ZmHttpdTask* task, const BYTE* data, size_t dlen);
	void SetupRouter();

	static const char* GetMimeType(const std::string& path);
	static std::string ExtractFilename(const std::string& uri);

	/** @brief 处理 HTTP Range 请求，返回 206 部分内容 */
	int ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
		const std::string& rangeStr, int64_t fileSize);

private:
	ZmEvBaseRunLoop*         m_evLoop;
	ZmHttpServer*            m_httpServer;
	std::string              m_wwwRoot;
	ZmHttpRouter             m_router;
	HttpServerModuleFileHub* m_fileHub;
};

#endif
