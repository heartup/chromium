# WebSocket MAC地址验证功能说明

## 功能概述
在WebSocket连接建立后，客户端会进行MAC地址验证：
1. 服务器发送一个MAC地址字符串给客户端
2. 客户端获取本机MAC地址并与服务器发送的进行比较
3. 只有验证通过后，客户端才能发送JSON消息给服务器

## 实现位置
- `/content/browser/websocket_client.cc` - Browser进程的WebSocket客户端实现
- `/content/browser/websocket_client.h` - 头文件定义

## 关键方法

### `GetLocalMacAddress()`
- 获取本机第一个有效网络接口的MAC地址
- 返回格式：`xx:xx:xx:xx:xx:xx`（小写十六进制）

### `VerifyMacAddress()`
- 连接建立后自动调用
- 从服务器接收MAC地址字符串
- 与本机MAC地址比较（忽略大小写）
- 发送验证结果给服务器

### `SendMessage()`
- 增加了MAC地址验证检查
- 只有`mac_verified_`为true时才能发送消息

## 验证流程
```
客户端连接 -> WebSocket握手 -> 接收服务器MAC地址 ->
获取本机MAC -> 比较验证 -> 发送验证结果 ->
(验证通过) -> 可以发送JSON消息
(验证失败) -> 断开连接
```

## 测试方法

### 1. 启动测试服务器
```bash
# 安装Python依赖
pip install websockets

# 编辑test_mac_websocket_server.py，修改TEST_MAC_ADDRESS为你的MAC地址
# 获取你的MAC地址：
ip link show  # Linux
ipconfig /all  # Windows

# 运行服务器
python test_mac_websocket_server.py
```

### 2. 编译Chrome
```bash
autoninja -C out/Default chrome
```

### 3. 运行Chrome并测试
```bash
./out/Default/chrome --json-websocket-port=8080
```

## 注意事项

1. **Linux平台限制**：当前Linux实现可能无法获取MAC地址，因为`NetworkInterface`结构在Linux上不填充`mac_address`字段。Windows平台完整支持。

2. **MAC地址格式**：使用冒号分隔的小写十六进制格式，例如：`aa:bb:cc:dd:ee:ff`

3. **安全考虑**：
   - MAC地址验证只是基础的身份验证
   - 不应作为唯一的安全措施
   - 建议配合其他认证机制使用

4. **错误处理**：
   - 如果无法获取本机MAC地址，连接将失败
   - 如果验证失败，连接将自动断开
   - 所有验证相关的日志会输出到Chrome日志中

## 调试
查看Chrome日志以了解验证过程：
```bash
./out/Default/chrome --enable-logging --v=1
```

日志中会显示：
- 本机MAC地址
- 服务器发送的MAC地址
- 验证结果