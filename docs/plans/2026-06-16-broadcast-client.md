# 广播客户端实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 ZmBroadcastClient，基于 libevent + ZmEvBaseRunLoop 连接广播服务端。

**Architecture:** 复用 base 层帧协议，内部自管理 ZmEvBaseRunLoop，connect 重试无限。消息路由：settings/ping 内部处理，业务消息投 ZmThreadPool 回调。

**Tech Stack:** C++17, libevent (bufferevent), ZmEvBaseRunLoop, ZmThreadPool, zm_json

---

### Task 1: 创建 zm_net_broadcast_client.h

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_client.h`
- Modify: `A:\ZiMo\ZiMoService\ZiMoService.vcxproj`

### Task 2: 创建 zm_net_broadcast_client.cpp

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_client.cpp`
- Modify: `A:\ZiMo\ZiMoService\ZiMoService.vcxproj`

### Task 3: 编译验证
