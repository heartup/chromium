# WebSocket密钥加密验证说明

## 概述
WebSocket连接使用加密密钥验证机制，确保客户端和服务器使用相同的密钥。

## 加密算法

### 算法步骤
1. **逐字符处理**：对密钥的每个字符进行变换
2. **位置异或**：每个字符与其位置索引进行XOR运算
3. **循环移位**：将字节循环左移3位
4. **固定值异或**：与0xA5进行XOR增加复杂度
5. **十六进制编码**：将结果转换为十六进制字符串

### 算法特点
- 简单高效，易于在C++和Python中实现
- 位置相关，相同字符在不同位置产生不同结果
- 输出长度是输入长度的2倍（十六进制编码）
- 不可逆，提供基本的安全性

## 验证流程

```
1. 客户端启动时获取密钥（命令行参数或默认值）
2. 客户端连接服务器
3. 服务器对自己的密钥进行加密
4. 服务器主动发送加密后的密钥给客户端
5. 客户端对本地密钥进行相同的加密
6. 客户端比较两个加密后的密钥
7. 如果匹配，客户端发送"AUTH_SUCCESS"给服务器
8. 如果不匹配，客户端发送"AUTH_FAILED"给服务器
9. 服务器根据客户端响应决定是否接受后续消息
```

## 使用方法

### Chrome客户端
```bash
# 使用默认密钥
./out/Default/chrome --json-websocket-port=7746

# 使用自定义密钥
./out/Default/chrome --json-websocket-port=7746 --json-websocket-key=your_secret_key
```

### Python服务器
```python
# 在json_ws_test_server.py中设置
TEST_AUTH_KEY = "878fddae9fe548cdb5b2939aa38d6cf3"  # 必须与客户端一致
```

### 默认密钥
- 默认值：`878fddae9fe548cdb5b2939aa38d6cf3`
- 加密后：`6414748ea6ae9eb62cdede54640ccefe368426accc94dc166ef4b45ef45666c4`

## 测试工具

### 验证加密算法
```bash
# 测试Python端的加密算法
python test_key_encryption.py
```

### 测试完整流程
```bash
# 1. 启动服务器
python json_ws_test_server.py

# 2. 启动Chrome（使用相同密钥）
./out/Default/chrome --json-websocket-key=878fddae9fe548cdb5b2939aa38d6cf3
```

## 调试信息

### Chrome日志
```
[WebSocket] Using auth key from command line: your_key
[WebSocket] Original key: your_key
[WebSocket] Encrypted key: encrypted_value
[WebSocket] Waiting for server auth key...
[WebSocket] Received encrypted auth key from server
[WebSocket] Server encrypted key: encrypted_value
[WebSocket] Local encrypted key: encrypted_value
[WebSocket] Auth key verification successful
[WebSocket] Sending AUTH_SUCCESS to server
```

### 服务器日志
```
=== 密钥加密验证 ===
服务器密钥: your_key
Original key: your_key
Encrypted key: encrypted_value
服务器加密后密钥: encrypted_value
发送加密密钥给客户端验证...
收到客户端验证响应: AUTH_SUCCESS
密钥验证成功! 客户端确认密钥匹配
```

## 安全说明

1. **传输安全**：只有加密后的密钥在网络上传输，原始密钥不会暴露
2. **服务器主导**：服务器主动发起验证，客户端本地验证，避免客户端伪造
3. **简单验证**：这是一个基础的验证机制，主要用于确保客户端和服务器配置一致
4. **生产环境**：建议配合TLS/SSL使用，提供更强的安全保护

## 故障排除

### 验证失败
- 检查客户端和服务器的密钥是否一致
- 查看日志中的加密值是否匹配
- 运行test_key_encryption.py验证算法正确性

### 常见错误
```
密钥验证失败! 密钥不匹配
服务器加密密钥: 6414748ea6ae9eb62cdede54640ccefe368426accc94dc166ef4b45ef45666c4
本地加密密钥: different_value
```
解决方法：确保--json-websocket-key参数与服务器TEST_AUTH_KEY一致

## 代码位置

- **C++实现**：`/content/browser/websocket_client.cc` - `EncryptKey()`函数
- **Python实现**：`/json_ws_test_server.py` - `encrypt_key()`函数
- **测试脚本**：`/test_key_encryption.py`