#ifndef ZM_NAME_DEFINE_H
#define ZM_NAME_DEFINE_H

//服务信息
#define SERVICE_NAME	_T("ZM_Svc")
#define SERVICE_DETAIL  _T("ZiMo的服务进程")
#define SERVICE_DESC    _T("承载ZiMo客户端的相关能力")

/** 通用 HTTP 服务器监听端口 */
#define ZM_HTTP_SERVER_PORT     80
/** 通用 HTTPS 服务器监听端口（启用 TLS 时使用，HTTP→HTTPS 重定向目标） */
#define ZM_HTTPS_SERVER_PORT    443

/** 本地 HTTP JRPC URI 路径 */
#define ZM_HTTP_JRPC_SERVER_ROOT_URI	"/zimo/jrpc"

#define ZM_HTTP_RESTFUL_SERVER_ROOT_URI	"/zimo/api"

/** HTTP JSON-RPC 服务器监听端口 */
#define ZM_JSONRPC_SERVER_PORT  39440

/** HTTP RESTful 服务器监听端口 */
#define ZM_RESTFUL_SERVER_PORT  39441

/** 广播服务端监听端口 */
#define ZM_BROADCAST_SERVER_PORT 39640


/** 文件中心 HMAC 密钥（用于密码哈希） */
#define ZM_FILE_HUB_HMAC_KEY    "ZiMoFileHub2024"
/** 文件中心数据目录（相对于 www 根目录） */
#define ZM_FILE_HUB_ROOT        "modules\\filehub"

#endif /* ZM_NAME_DEFINE_H */