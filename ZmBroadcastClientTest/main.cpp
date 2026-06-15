/**
 * @file main.cpp
 * @brief TCP 广播客户端测试程序
 *
 * 连接到本地广播服务端 (127.0.0.1:39640)，
 * 测试连接、握手、心跳、消息接收和 tag 订阅功能。
 */

#include "zm_net_broadcast_client.h"
#include "zm_net_socket.h"
#include "zm_util_thread.h"
#include "zm_logger.h"

#include <cstdio>
#include <csignal>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

static bool g_running = true;

// ============================================================================
// 业务层消息回调 — 线程池线程中执行
// ============================================================================

void OnMessage(const std::string& topic, const std::string& content)
{
	printf("\n[业务层] 收到消息:\n");
	printf("  topic:   %s\n", topic.c_str());
	printf("  content: %s\n", content.c_str());
}

// ============================================================================
// 连接事件回调
// ============================================================================

void OnConnected(const std::string& ip, uint16_t port)
{
	printf("[回调] 连接成功: %s:%u\n", ip.c_str(), port);
}

void OnConnectFailed(const std::string& error)
{
	printf("[回调] 连接失败: %s\n", error.c_str());
}

void OnDisconnected()
{
	printf("[回调] 连接断开\n");
}

void OnError(const std::string& error)
{
	printf("[回调] 错误: %s\n", error.c_str());
}

// ============================================================================
// 主函数
// ============================================================================

int main()
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	printf("=== ZmBroadcastClient 测试程序 ===\n\n");

	// 0. 初始化 WinSock
	ZmWinSockHelper::Init();

	// 1. 创建线程池（用于业务消息回调）
	ZmThreadPool threadPool(2);

	// 2. 配置客户端
	BcClientConfig cfg;
	cfg.serverIp        = "127.0.0.1";
	cfg.serverPort      = 39640;
	cfg.handshakeTimeout = 10;
	cfg.initialTags     = { "test", "demo" };
	cfg.threadPool      = &threadPool;

	// 3. 设置回调
	BcClientCallbacks cbs;
	cbs.onConnected     = OnConnected;
	cbs.onConnectFailed = OnConnectFailed;
	cbs.onDisconnected  = OnDisconnected;
	cbs.onError         = OnError;
	cbs.onMessage       = OnMessage;

	// 4. 创建客户端并连接
	ZmBroadcastClient client(cfg, cbs);

	printf("正在连接 %s:%u ...\n", cfg.serverIp.c_str(), cfg.serverPort);

	if (!client.Connect())
	{
		printf("连接启动失败\n");
		return -1;
	}

	// 5. 等握手完成
	printf("等待握手完成...\n");
	{
		auto start = std::chrono::steady_clock::now();
		while (client.GetState() != ZM_BC_STATE_LISTENING)
		{
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - start).count();
			if (elapsed > 15)
			{
				printf("握手超时（服务端可能未启动）\n");
				client.Disconnect();
				return -1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
	}

	printf("\n连接就绪！\n");
	printf("服务端: %s:%u\n", client.GetServerIp().c_str(), client.GetServerPort());
	printf("当前 tag: ");
	for (auto& t : client.GetTags())
		printf("%s ", t.c_str());
	printf("\n\n");

	// 6. 测试：新增订阅
	printf("=== 测试: 新增订阅 tag3 ===\n");
	client.Subscribe({ "tag3" });
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	// 7. 测试：取消订阅
	printf("=== 测试: 取消订阅 test ===\n");
	client.Unsubscribe({ "test" });
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	// 8. 等待接收消息（服务端可通过 JRPC broadcast 方法推送）
	printf("\n=== 等待业务消息（按 Ctrl+C 退出）===\n");
	printf("提示: 通过 JRPC 发送广播来测试:\n");
	printf("  curl -X POST http://127.0.0.1:39440/ZiMo/JRPC\n");
	printf("    -d '{\"method\":\"broadcast\",\"params\":{\"topic\":\"hello\",\"content\":\"{\\\"msg\\\":\\\"world\\\"}\",\"tag\":\"demo\"}}'\n\n");

	while (g_running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		// 打印运行状态
		printf("\r运行中... 累计接收: %llu 条",
			(unsigned long long)client.GetReceivedCount());
		fflush(stdout);
	}

	// 9. 断开
	printf("\n断开连接...\n");
	client.Disconnect();
	printf("退出\n");

	return 0;
}
