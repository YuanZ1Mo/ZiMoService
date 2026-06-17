#ifndef HTTP_SERVER_MODULE_FILE_HUB_H
#define HTTP_SERVER_MODULE_FILE_HUB_H

#include "zm_json.h"
#include "zm_net_http_router.h"

#include <string>
#include <vector>

class HttpServerManager;

/**
 * @brief 文件中心模块
 *
 * 管理 www/module_file_hub/ 下的文件/文件夹，提供列表、搜索、创建、
 * 删除、密码验证等功能。每个文件目录可配置独立的用户名/密码（HMAC 哈希存储）。
 */
class HttpServerModuleFileHub
{
public:
	/**
	 * @brief 构造文件中心模块
	 * @param wwwRoot www 根目录绝对路径
	 */
	explicit HttpServerModuleFileHub(const std::string& wwwRoot);
	~HttpServerModuleFileHub();

	/**
	 * @brief 注册文件中心 HTTP 路由（/file_hub/download/*、/file_hub/upload/*）
	 * @param router  HTTP 路由器
	 * @param httpMgr HTTP 服务器管理器（用于通用的 SendFile / ReceiveFile）
	 */
	void RegisterHttpRoutes(ZmHttpRouter& router, HttpServerManager* httpMgr);

	/**
	 * @brief 列出指定路径下的一层文件和文件夹
	 * @param relativePath 相对于文件中心根目录的路径（空字符串表示根）
	 * @return {ok: true, files: [{name, size, type, hasChild}, ...]}
	 */
	ZMJSON ListFiles(const std::string& relativePath);

	/**
	 * @brief 模糊搜索文件/文件夹名
	 * @param keyword 搜索关键词
	 * @return {ok: true, results: [fullPath, ...]}
	 */
	ZMJSON SearchFiles(const std::string& keyword);

	/**
	 * @brief 创建目录（文件夹），可选设置用户名/密码
	 * @param parentPath 父目录的相对路径
	 * @param dirName    新目录名称
	 * @param username   用户名（可为空）
	 * @param password   密码（可为空，非空时与 username 一起写入 .userConfig）
	 * @return {ok: true} 或 {ok: false, error: "..."}
	 */
	ZMJSON CreateDir(const std::string& parentPath, const std::string& dirName,
		const std::string& username = "", const std::string& password = "");

	/**
	 * @brief 删除文件或空文件夹
	 * @param relativePath 要删除的文件/文件夹的相对路径
	 * @param username     用户名（有密码保护的目录需要）
	 * @param password     密码（有密码保护的目录需要）
	 * @return {ok: true} 或 {ok: false, error: "..."}
	 */
	ZMJSON DeleteItem(const std::string& relativePath,
		const std::string& username = "", const std::string& password = "");

	/**
	 * @brief 验证目录密码
	 * @param relativePath 目录的相对路径
	 * @param password     输入的密码
	 * @return {ok: true, valid: true/false}
	 */
	ZMJSON VerifyDirPassword(const std::string& relativePath,
		const std::string& password);

	/**
	 * @brief 修改目录密码
	 * @param relativePath 目录的相对路径
	 * @param username     用户名
	 * @param oldPassword  旧密码（首次设置密码时可为空）
	 * @param newPassword  新密码
	 * @return {ok: true} 或 {ok: false, error: "..."}
	 */
	ZMJSON ChangeDirPassword(const std::string& relativePath,
		const std::string& username, const std::string& oldPassword,
		const std::string& newPassword);

	/**
	 * @brief 批量删除文件
	 * @param paths      要删除的文件路径数组
	 * @param username   用户名（受密码保护的目录需要）
	 * @param password   密码
	 * @return {ok: true, deleted: n} 或 {ok: false, error: "..."}
	 */
	ZMJSON BatchDelete(const ZMJSON& paths,
		const std::string& username = "", const std::string& password = "");

private:
	/** @brief 获取文件中心根目录的绝对路径 */
	std::string GetHubRoot() const;

	/**
	 * @brief 将相对路径转为规范化绝对路径，并校验路径穿越
	 * @param relativePath 相对路径
	 * @param absPath      [out] 规范化后的绝对路径
	 * @return true 成功
	 */
	bool NormalizeHubPath(const std::string& relativePath, std::string& absPath);

	/** @brief 读取目录下的 .userConfig */
	bool ReadUserConfig(const std::string& dirAbsPath, ZMJSON& config);

	/** @brief 写入目录下的 .userConfig */
	bool WriteUserConfig(const std::string& dirAbsPath, const ZMJSON& config);

	/** @brief 验证密码是否匹配 config 中的 password_hash */
	bool VerifyPassword(const ZMJSON& config, const std::string& password);

	/** @brief 使用 HMAC-SHA256 对密码做哈希 */
	std::string HashPassword(const std::string& password);

	/** @brief 递归搜索辅助函数 */
	void SearchRecursive(const std::string& absDir, const std::string& relativeDir,
		const std::string& keyword, std::vector<std::string>& results);

private:
	std::string m_wwwRoot;  ///< www 根目录绝对路径
};

#endif // HTTP_SERVER_MODULE_FILE_HUB_H
