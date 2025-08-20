# JSON WebSocket Hook Implementation

这个模块实现了在Blink层对JSON.stringify的钩子，能够通过WebSocket发送JSON消息。

## 方案概述

1. **JSONStringifyHook**: 在`bindings/core/v8/`中创建的集中式JSON.stringify包装器
2. **JSONWebSocketSender**: 在`modules/json_ws/`中的WebSocket发送器
3. **JSONWebSocketController**: JavaScript接口，允许启用/禁用功能

## 文件结构

```
third_party/blink/renderer/
├── bindings/core/v8/
│   ├── json_stringify_hook.h     # JSONStringifyHook类声明
│   └── json_stringify_hook.cc    # JSONStringifyHook实现
└── modules/json_ws/
    ├── BUILD.gn                              # 构建配置
    ├── README.md                             # 本文档
    ├── json_ws_controller.h                  # JavaScript控制器
    ├── json_ws_controller.cc
    ├── json_ws_controller.idl                # IDL接口定义
    ├── json_ws_controller_global_scope.h     # Window接口扩展
    ├── json_ws_controller_global_scope.cc
    ├── json_ws_controller_global_scope.idl
    ├── json_ws_sender.h                      # WebSocket发送器
    └── json_ws_sender.cc
```

## 使用方法

### 启用钩子

在JavaScript中：

```javascript
// 启用JSON stringify hook，连接到WebSocket服务器
window.__jsonWsController.enableHook('ws://localhost:8080/json-hook');

// 检查是否启用
console.log(window.__jsonWsController.isHookEnabled()); // true

// 现在所有的JSON.stringify调用都会通过WebSocket发送
const data = {message: "Hello", timestamp: Date.now()};
JSON.stringify(data); // 这会触发hook并发送到WebSocket
```

### 禁用钩子

```javascript
// 禁用钩子
window.__jsonWsController.disableHook();
```

## 集成示例

要在现有代码中使用hook，替换直接的`v8::JSON::Stringify`调用：

**修改前**:
```cpp
#include "v8/include/v8.h"

// 在某个函数中
v8::Local<v8::String> result;
if (v8::JSON::Stringify(context, json_object).ToLocal(&result)) {
    // 处理结果
}
```

**修改后**:
```cpp
#include "third_party/blink/renderer/bindings/core/v8/json_stringify_hook.h"

// 在某个函数中  
v8::Local<v8::String> result;
if (JSONStringifyHook::Stringify(context, json_object).ToLocal(&result)) {
    // 处理结果 - 如果hook启用，JSON也会通过WebSocket发送
}
```

## 已知集成点

以下文件中的`v8::JSON::Stringify`调用可以替换为使用hook：

- `core/timing/performance_user_timing.cc`: 性能标记的详细信息序列化
- `bindings/core/v8/v8_object_builder_test.cc`: 测试代码
- `bindings/core/v8/serialization/v8_script_value_serializer_test.cc`: 序列化测试

## 构建配置

需要在相关的BUILD.gn文件中添加依赖：

```gn
deps = [
  "//third_party/blink/renderer/modules/json_ws",
  # 其他依赖...
]
```

## 运行时特性标志

该功能通过`RuntimeEnabled=JSONWebSocketHook`特性标志控制，可以在运行时启用或禁用。

## 安全考虑

- 只在调试/开发构建中启用此功能
- WebSocket连接应当受到适当的安全限制
- 考虑对发送的JSON数据进行大小限制，防止内存或网络滥用

## 调试信息

启用此功能后，所有通过`JSONStringifyHook::Stringify`处理的JSON数据都会：
1. 正常返回给调用方（保持原有语义）
2. 如果hook启用且WebSocket连接正常，同时发送到配置的WebSocket服务器

这为调试复杂的web应用提供了一个强大的工具，可以实时监控所有的JSON序列化操作。
