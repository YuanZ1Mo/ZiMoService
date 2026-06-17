#ifndef ZM_NAME_DEFINE_H
#define ZM_NAME_DEFINE_H

//服务信息
#define SERVICE_NAME	_T("ZM_Svc")
#define SERVICE_DETAIL  _T("ZiMo的服务进程")
#define SERVICE_DESC    _T("承载ZiMo客户端的相关能力")

/** 通用 HTTP 服务器监听端口 */
#define ZM_HTTP_SERVER_PORT     80

/** 本地 HTTP URI 路径 */
#define ZM_HTTPSERVER_ROOT_URI	"/ZiMo/JRPC"

/** HTTP JSON-RPC 服务器监听端口 */
#define ZM_JSONRPC_SERVER_PORT  39440

/** 广播服务端监听端口 */
#define ZM_BROADCAST_SERVER_PORT 39640

/** SOCKS5 服务器监听端口 */
#define ZM_SOCKS5_SERVER_PORT   39540

/** JRPC 协议帧魔数（4 字节） */
#define ZM_JRPC_MAGIC           "JRPC"

/** JRPC 错误码：SendToHubProxy 失败 */
#define ZM_JRPC_ERR_SEND_HUB    601
/** JRPC 错误码：响应为空 */
#define ZM_JRPC_ERR_EMPTY_RSP   602
/** JRPC 错误码：响应格式错误 */
#define ZM_JRPC_ERR_FORMAT      603

/** 文件中心 HMAC 密钥（用于密码哈希） */
#define ZM_FILE_HUB_HMAC_KEY    "ZiMoFileHub2024"
/** 文件中心数据目录（相对于 www 根目录） */
#define ZM_FILE_HUB_ROOT        "db\\filehub"

#endif /* ZM_NAME_DEFINE_H */