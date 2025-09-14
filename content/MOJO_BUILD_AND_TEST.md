# Mojo IPC WebSocket 编译和测试指南

## 编译步骤

### 1. 生成 Mojo 绑定代码

```bash
# 先生成 Mojo 接口的 C++ 绑定
autoninja -C out/Default content/common:mojo_bindings
```

### 2. 编译 Chrome

```bash
# 完整编译 Chrome
autoninja -C out/Default chrome
```

如果编译出错，可能需要：

### 3. 可能的编译错误修复

#### 错误: 找不到 mojom 头文件
```bash
# 确保 Mojo 绑定生成成功
gn gen out/Default
autoninja -C out/Default content/common:mojo_bindings
```

#### 错误: 未定义的符号
确保所有文件都已添加到 BUILD.gn：
- `content/browser/BUILD.gn` 包含 `json_websocket_service_impl.*` 和 `websocket_client.*`
- `content/renderer/BUILD.gn` 包含 `json_websocket_client.*`
- `content/common/BUILD.gn` 包含 `json_websocket.mojom`

## 测试步骤

### 1. 启动 WebSocket 测试服务器

创建一个简单的 Python WebSocket 服务器 `test_server.py`：

```python
import asyncio
import websockets
import json
from datetime import datetime

async def handle_client(websocket, path):
    print(f"Client connected from {websocket.remote_address}")
    try:
        async for message in websocket:
            print(f"[{datetime.now()}] Received: {message[:200]}...")
            # 解析 JSON
            try:
                data = json.loads(message)
                print(f"  Type: {data.get('type', 'unknown')}")
            except:
                pass
            # 发送确认
            await websocket.send(json.dumps({"status": "received"}))
    except websockets.exceptions.ConnectionClosed:
        print("Client disconnected")

async def main():
    print("Starting WebSocket server on 127.0.0.1:8080")
    async with websockets.serve(handle_client, "127.0.0.1", 8080):
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    asyncio.run(main())
```

运行服务器：
```bash
pip3 install websockets
python3 test_server.py
```

### 2. 启动 Chrome（无需 --no-sandbox！）

```bash
# 启用详细日志，正常沙盒模式
./out/Default/chrome \
  --enable-logging \
  --v=1 \
  --vmodule=*json*=2,*websocket*=2 \
  2>&1 | tee chrome.log
```

### 3. 访问测试页面

创建测试 HTML 文件 `test.html`：

```html
<!DOCTYPE html>
<html>
<head>
    <title>WebSocket IPC Test</title>
</head>
<body>
    <h1>WebSocket IPC Test</h1>
    <button onclick="testJSON()">Send Test JSON</button>
    <div id="output"></div>

    <script>
    function testJSON() {
        // 触发包含关键字的 JSON.stringify
        const data = {
            type: "WP_dealNotify",
            timestamp: Date.now(),
            message: "Test message from Chrome"
        };

        const jsonStr = JSON.stringify(data);
        document.getElementById('output').innerHTML +=
            '<p>Sent: ' + jsonStr + '</p>';

        console.log('JSON stringified:', jsonStr);
    }

    // 自动测试
    setInterval(() => {
        const testData = {
            type: "WP_userOptNotify",
            action: "auto_test",
            time: new Date().toISOString()
        };
        JSON.stringify(testData);
    }, 5000);
    </script>
</body>
</html>
```

用 Chrome 打开这个文件。

### 4. 验证工作状态

#### 检查日志

```bash
# 查看 Chrome 日志
grep -E "JSONMonitor|JsonWebSocket|IPC" chrome.log

# 应该看到类似：
# [JSONMonitor] Initializing WebSocket via Mojo IPC to 127.0.0.1:8080
# [Renderer] Requesting connection to 127.0.0.1:8080
# [Browser] IPC: Connect request received
# [Browser] WebSocket connected successfully
# [JSONMonitor] WebSocket connection established via Mojo IPC (works in sandbox!)
# [JSONMonitor] JSON sent successfully via Mojo IPC
```

#### 检查服务器端

在 Python 服务器终端应该看到：
```
Client connected from ('127.0.0.1', xxxxx)
[2024-xx-xx] Received: {"type":"WP_dealNotify"...
  Type: WP_dealNotify
```

## 调试技巧

### 1. 启用更详细的日志

```bash
./out/Default/chrome \
  --enable-logging \
  --v=2 \
  --vmodule=*json*=3,*websocket*=3,*mojo*=2,browser_interface_binders=3
```

### 2. 检查 Mojo 连接

如果 IPC 不工作，检查：
- 服务是否在 `browser_interface_binders.cc` 中注册
- Mojo 接口是否正确生成（检查 `out/Default/gen/content/common/`）

### 3. 使用 Chrome DevTools

打开 `chrome://inspect` 查看渲染进程的控制台输出。

### 4. 检查进程隔离

```bash
# 确认渲染进程在沙盒中运行
ps aux | grep chrome
# 渲染进程应该有 --type=renderer 参数
```

## 常见问题

### Q: 编译失败，找不到 mojom 头文件
A: 运行 `autoninja -C out/Default content/common:mojo_bindings` 生成绑定。

### Q: WebSocket 连接失败
A: 检查：
1. Python 服务器是否运行在 127.0.0.1:8080
2. 防火墙设置
3. Chrome 日志中的错误信息

### Q: IPC 调用没有响应
A: 检查：
1. 服务是否正确注册
2. 浏览器进程日志
3. Mojo 连接状态

### Q: 如何确认在沙盒模式下运行？
A: 不使用 `--no-sandbox` 参数启动 Chrome。如果 WebSocket 仍能工作，说明 IPC 方案成功。

## 性能测试

```javascript
// 在控制台运行性能测试
console.time('ipc-test');
for(let i = 0; i < 100; i++) {
    JSON.stringify({
        type: "WP_dealNotify",
        index: i,
        data: "x".repeat(1000)
    });
}
console.timeEnd('ipc-test');
```

预期：IPC 开销应该在毫秒级别，对性能影响很小。