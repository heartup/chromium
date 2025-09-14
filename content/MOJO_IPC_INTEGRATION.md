# Mojo IPC WebSocket 集成指南

## 概述

这个解决方案通过 Mojo IPC 让渲染进程能够在沙盒模式下发送 WebSocket 消息。渲染进程通过 IPC 将消息发送到浏览器进程，由浏览器进程（不受沙盒限制）建立实际的 WebSocket 连接。

## 架构

```
[渲染进程 (沙盒中)]
    ↓
[JSONMonitor] → [JsonWebSocketClient]
    ↓
[Mojo IPC]
    ↓
[浏览器进程 (无沙盒)]
    ↓
[JsonWebSocketServiceImpl] → [WebSocketClient]
    ↓
[WebSocket Server (127.0.0.1:8080)]
```

## 文件结构

### 1. Mojo 接口定义
- `/content/common/json_websocket.mojom` - Mojo 接口定义

### 2. 浏览器进程实现
- `/content/browser/json_websocket_service_impl.h` - 服务接口
- `/content/browser/json_websocket_service_impl.cc` - 服务实现

### 3. 渲染进程客户端
- `/content/renderer/json_websocket_client.h` - 客户端接口
- `/content/renderer/json_websocket_client.cc` - 客户端实现

### 4. JSONMonitor 集成
- `/content/renderer/json_monitor_observer.cc` - 已修改为使用 Mojo IPC

## 需要的额外集成步骤

### 1. 在浏览器进程注册服务

需要在 `content/browser/browser_interface_binders.cc` 中添加服务绑定：

```cpp
// 在 PopulateFrameBinders 函数中添加：
registry->AddInterface(
    base::BindRepeating(&JsonWebSocketServiceImpl::Create));
```

### 2. 更新 BUILD.gn 文件

#### content/browser/BUILD.gn
添加到 sources 列表：
```gn
sources += [
  "json_websocket_service_impl.cc",
  "json_websocket_service_impl.h",
]
```

#### content/renderer/BUILD.gn
添加到 sources 列表：
```gn
sources += [
  "json_websocket_client.cc",
  "json_websocket_client.h",
]
```

### 3. 复制 WebSocket 客户端实现

由于浏览器进程需要使用 WebSocketClient，需要：
1. 将 `websocket_client.cc/h` 复制到 `content/browser/` 目录
2. 或者创建一个共享库供两个进程使用

## 编译步骤

```bash
# 1. 生成 Mojo 绑定
autoninja -C out/Default content/common:mojo_bindings

# 2. 编译完整的 Chrome
autoninja -C out/Default chrome
```

## 测试方法

### 1. 启动 WebSocket 服务器
```python
# 在 127.0.0.1:8080 启动你的 WebSocket 服务器
python3 websocket_server.py
```

### 2. 启动 Chrome（无需 --no-sandbox！）
```bash
./out/Default/chrome --enable-logging --v=1
```

### 3. 访问触发 JSON.stringify 的网页
打开包含你关注的 JSON 数据的网页

### 4. 检查日志
```bash
# 查看 Chrome 日志
tail -f chrome_debug.log | grep JSONMonitor

# 应该看到类似：
# [JSONMonitor] WebSocket connection established via Mojo IPC (works in sandbox!)
# [JSONMonitor] JSON sent successfully via Mojo IPC
```

## 优势

1. **安全性**：保持 Chrome 沙盒完整性
2. **架构正确**：遵循 Chrome 的设计原则
3. **可靠性**：使用标准 IPC 机制
4. **性能**：Mojo IPC 高效且异步

## 调试提示

1. 启用详细日志：`--enable-logging --v=2`
2. 检查 Mojo 连接：在日志中搜索 "JsonWebSocketService"
3. 验证 IPC 通信：查看 "[Browser]" 和 "[Renderer]" 前缀的日志

## 常见问题

### Q: 为什么选择 Mojo IPC？
A: Mojo 是 Chrome 的官方 IPC 机制，专门设计用于处理沙盒限制。

### Q: 性能影响如何？
A: Mojo IPC 开销很小，消息传递是异步的，不会阻塞渲染进程。

### Q: 可以处理大量数据吗？
A: 是的，Mojo 支持大消息和流式传输。

## 下一步

1. 完成浏览器进程的服务注册
2. 更新 BUILD.gn 文件
3. 编译并测试
4. 考虑添加连接池支持多个 WebSocket 连接