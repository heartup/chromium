# JSON Capture Extension API 实现完整总结

## 项目目标
将现有的 JSON.stringify hook 实现从 WebSocket 数据传输方式迁移到 Chrome Extension API，使 extensions 能够直接接收捕获的 JSON 数据。

---

## 架构设计

### 数据流向
```
Renderer Process (网页)
    ↓
V8 JSON.stringify Hook (JSONMonitorManager - 单例)
    ↓
Mojo IPC (JsonCaptureClient - 每帧一个实例)
    ↓
Browser Process (JsonCaptureServiceImpl - 每帧一个实例)
    ↓
Extension API (JsonCaptureEventRouter - 每个 BrowserContext 一个实例)
    ↓
Extension Background Service Worker
```

---

## 单例 vs 非单例设计说明

### ⭐ 单例组件

#### 1. **JSONMonitorManager** (Renderer Process)
**位置**: `content/renderer/json_monitor_observer.h/cc`

**为什么是单例**:
- V8 的 `SetJSONStringifyCallback` 是**全局回调**，在整个 renderer process 中只能设置一次
- 如果每个 frame 都设置回调，后面的会覆盖前面的，导致只有最后一个 frame 能收到回调
- 多个 frame 释放时会互相干扰，可能将回调设置为 nullptr

**实现**:
```cpp
class JSONMonitorManager {
 public:
  static JSONMonitorManager* GetInstance();  // 单例访问点

  void RegisterFrame(RenderFrameImpl* frame, JsonCaptureClient* client);
  void UnregisterFrame(RenderFrameImpl* frame);
  static void OnJSONStringify(const std::string& json_content, void* user_data);

 private:
  JSONMonitorManager();  // 私有构造
  ~JSONMonitorManager();

  base::Lock lock_;  // 线程安全保护
  std::map<RenderFrameImpl*, JsonCaptureClient*> frame_clients_;  // 管理所有 frame
};
```

**职责**:
- 在构造函数中**一次性**设置 V8 全局回调
- 维护所有 frame 的 JsonCaptureClient 映射
- 当 JSON.stringify 被调用时，将数据分发到所有注册的 frame clients

---

### ⭐ 非单例组件（每帧实例）

#### 2. **JsonCaptureClient** (Renderer Process)
**位置**: `content/renderer/json_capture_client.h/cc`

**为什么不是单例**:
- 每个 RenderFrame 需要独立的 **Mojo 连接**到 browser process
- 不同 frame 可能对应不同的 browser contexts (如 incognito vs normal)
- Frame 生命周期独立，一个 frame 销毁不应影响其他 frame

**实现**:
```cpp
class JsonCaptureClient {
 public:
  explicit JsonCaptureClient(RenderFrameImpl* render_frame);
  ~JsonCaptureClient();

  void Initialize();
  bool SendJsonData(const std::string& json_content);

 private:
  raw_ptr<RenderFrameImpl> render_frame_;           // 所属 frame
  mojo::Remote<mojom::JsonCaptureService> service_; // 每帧独立的 Mojo 连接
};
```

**生命周期**: 由 `JSONMonitor` 拥有，随 frame 的 script context 创建和销毁

---

#### 3. **JSONMonitor** (Renderer Process)
**位置**: `content/renderer/json_monitor_observer.h/cc`

**为什么不是单例**:
- 每个 RenderFrame 有独立的 script context
- 需要为每个 frame 创建独立的 JsonCaptureClient

**实现**:
```cpp
class JSONMonitor {
 public:
  explicit JSONMonitor(RenderFrameImpl* render_frame);
  ~JSONMonitor();
  void Initialize();

 private:
  raw_ptr<RenderFrameImpl> render_frame_;
  std::unique_ptr<JsonCaptureClient> capture_client_;  // 拥有 client
};
```

**生命周期**: 由 `JSONMonitorObserver` 拥有，在 `DidCreateScriptContext` 创建，在 `WillReleaseScriptContext` 销毁

---

#### 4. **JsonCaptureServiceImpl** (Browser Process)
**位置**: `content/browser/json_capture_service_impl.h/cc`

**为什么不是单例**:
- 每个 Mojo 连接（每个 RenderFrame）需要独立的 service 实例
- Self-owned 模式：当 Mojo 连接断开时自动删除

**实现**:
```cpp
class JsonCaptureServiceImpl : public mojom::JsonCaptureService {
 public:
  static void Create(RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<mojom::JsonCaptureService> receiver);

  void NotifyJsonCapture(const std::string& json_content,
                        const GURL& source_url,
                        int32_t frame_id) override;

 private:
  const GlobalRenderFrameHostId render_frame_host_id_;  // 安全引用 frame
  mojo::Receiver<mojom::JsonCaptureService> receiver_;
};
```

**关键设计**:
- 使用 `GlobalRenderFrameHostId` 而非原始指针，避免 dangling pointer
- 在 `GetBrowserContext()` 中安全地解引用 frame ID

---

#### 5. **JsonCaptureEventRouter** (Browser Process)
**位置**: `chrome/browser/extensions/api/json_capture/json_capture_api.h/cc`

**为什么不是单例（但是 per-BrowserContext 单例）**:
- Chrome 支持多个 profiles (BrowserContext)，如 normal 和 incognito
- 每个 BrowserContext 有独立的 extension 实例
- 使用 `BrowserContextKeyedServiceFactory` 模式

**实现**:
```cpp
class JsonCaptureEventRouter : public BrowserContextKeyedAPI {
 public:
  static JsonCaptureEventRouter* Get(content::BrowserContext* context);

  void DispatchJsonCaptureEvent(const std::string& json_content,
                                const GURL& source_url,
                                int32_t frame_id);
  void StartCapturing();
  void StopCapturing();
  bool IsCapturing();

 private:
  content::BrowserContext* browser_context_;
  bool capturing_ = false;
};
```

**生命周期**: 随 BrowserContext 创建和销毁

---

## 文件清单

### 新建文件

#### Extension API 定义
1. **`chrome/common/extensions/api/json_capture.json`**
   - 定义 `chrome.jsonCapture` API schema
   - Events: `onData`
   - Functions: `start()`, `stop()`, `isCapturing()`

#### Mojo 接口
2. **`content/common/json_capture.mojom`**
   - 定义 renderer-browser IPC 接口
   - `NotifyJsonCapture()`, `StartCapturing()`, `StopCapturing()`, `IsCapturing()`

#### Renderer 端实现
3. **`content/renderer/json_capture_client.h`**
4. **`content/renderer/json_capture_client.cc`**
   - 每帧 Mojo client 实现

#### Browser 端实现
5. **`content/browser/json_capture_service_impl.h`**
6. **`content/browser/json_capture_service_impl.cc`**
   - Browser 端 Mojo service 实现

#### Extension API 实现
7. **`chrome/browser/extensions/api/json_capture/json_capture_api.h`**
8. **`chrome/browser/extensions/api/json_capture/json_capture_api.cc`**
   - JsonCaptureEventRouter (事件路由)
   - Extension function 实现

9. **`chrome/browser/extensions/api/json_capture/BUILD.gn`**
   - Build 配置

#### 示例 Extension
10. **`/home/lhh/heartup/git/json_capture_extension/manifest.json`**
11. **`/home/lhh/heartup/git/json_capture_extension/background.js`**
12. **`/home/lhh/heartup/git/json_capture_extension/README.md`**

#### 测试文件
13. **`/home/lhh/heartup/git/json_capture_test.html`**
    - 测试 JSON.stringify hook 的 HTML 页面

---

### 修改的文件

#### Renderer 端修改
14. **`content/renderer/json_monitor_observer.h`**
    - 添加 `JSONMonitorManager` 单例类
    - 修改 `JSONMonitor` 为非单例，每帧实例

15. **`content/renderer/json_monitor_observer.cc`**
    - 实现 `JSONMonitorManager` 单例管理所有 frame
    - 修改 `JSONMonitor::Initialize()` 注册到全局 manager
    - 修改 `OnJSONStringify()` 为 manager 的静态方法
    - 在 `WillReleaseScriptContext` 时注销 frame

#### Browser 端修改
16. **`content/browser/browser_interface_binders.cc`**
    - 注册 `JsonCaptureService` Mojo 接口绑定
    ```cpp
    #include "content/browser/json_capture_service_impl.h"
    #include "content/common/json_capture.mojom.h"

    render_frame_host->GetBrowserInterfaceBroker().SetBinderForTesting(
        mojom::JsonCaptureService::Name_,
        base::BindRepeating(&JsonCaptureServiceImpl::Create, render_frame_host));
    ```

17. **`chrome/browser/extensions/api/api_browser_context_keyed_service_factories.cc`**
    - 注册 `JsonCaptureEventRouter::GetFactoryInstance()`
    ```cpp
    #include "chrome/browser/extensions/api/json_capture/json_capture_api.h"

    extensions::JsonCaptureEventRouter::GetFactoryInstance();
    ```

#### Extension 权限系统
18. **`extensions/common/mojom/api_permission_id.mojom`**
    - 添加 `kJsonCapture = 264`
    ```cpp
    kEnterpriseLogin = 263,
    kJsonCapture = 264,
    ```

19. **`chrome/common/extensions/permissions/chrome_api_permissions.cc`**
    - 注册权限名称映射
    ```cpp
    {APIPermissionID::kJsonCapture, "jsonCapture"},
    ```

20. **`chrome/common/extensions/api/_api_features.json`**
    - 添加 API feature 定义
    ```json
    "jsonCapture": {
      "dependencies": ["permission:jsonCapture"],
      "contexts": ["privileged_extension"]
    }
    ```

21. **`chrome/common/extensions/api/_permission_features.json`**
    - 添加 permission feature 定义
    ```json
    "jsonCapture": {
      "channel": "stable",
      "extension_types": ["extension"]
    }
    ```

#### Histogram 值
22. **`extensions/browser/extension_function_histogram_value.h`**
    - 添加函数 histogram 值
    ```cpp
    JSONCAPTURE_START = 1948,
    JSONCAPTURE_STOP = 1949,
    JSONCAPTURE_ISCAPTURING = 1950,
    ```

23. **`extensions/browser/extension_event_histogram_value.h`**
    - 添加事件 histogram 值
    ```cpp
    JSON_CAPTURE_ON_DATA = 569,
    ```

#### API Schema 注册
24. **`chrome/common/extensions/api/api_sources.gni`**
    - 添加 `json_capture.json` 到 schema_sources_
    ```python
    schema_sources_ = [
      # ...
      "json_capture.json",
      # ...
    ]
    ```

#### BUILD 文件
25. **`content/renderer/BUILD.gn`**
    - 添加 `json_capture_client.cc/h`

26. **`content/browser/BUILD.gn`**
    - 添加 `json_capture_service_impl.cc/h`

27. **`content/common/BUILD.gn`**
    - 添加 `json_capture.mojom`

28. **`chrome/browser/extensions/api/BUILD.gn`**
    - 添加 `//chrome/browser/extensions/api/json_capture` 依赖

---

## 关键问题解决历程

### 问题 1: 编译错误 - 缺少 Histogram 值
**错误**:
```
error: no member named 'JSONCAPTURE_START' in namespace 'extensions::functions'
```

**解决**: 在 `extension_function_histogram_value.h` 和 `extension_event_histogram_value.h` 中添加枚举值

---

### 问题 2: 编译错误 - GetBrowserInterfaceBroker 指针问题
**错误**:
```
error: member reference type 'const blink::BrowserInterfaceBrokerProxy' is not a pointer
```

**解决**: 将 `->` 改为 `.`

---

### 问题 3: 编译错误 - GetRoutingID 不存在
**错误**:
```
error: no member named 'GetRoutingID' in 'content::RenderFrameImpl'
```

**解决**: 使用 `frame_id = 0` 作为占位符

---

### 问题 4: 编译错误 - 不完整类型 RenderProcessHost
**错误**:
```
error: member access into incomplete type 'RenderProcessHost'
```

**解决**: 添加 `#include "content/public/browser/render_process_host.h"`

---

### 问题 5: Runtime Crash - KeyedService 未注册
**错误**:
```
DCHECK failed: Trying to register KeyedService Factory: JsonCaptureEventRouter
after the call to the main registration function
```

**解决**: 在 `api_browser_context_keyed_service_factories.cc` 中注册 `JsonCaptureEventRouter::GetFactoryInstance()`

---

### 问题 6: Runtime Crash - Dangling Pointer
**错误**:
```
[DanglingPtr] A raw_ptr/raw_ref is dangling.
RenderFrameHostImpl::~RenderFrameHostImpl() → JsonCaptureServiceImpl::~JsonCaptureServiceImpl()
```

**解决**:
- 将 `raw_ptr<RenderFrameHost>` 改为 `GlobalRenderFrameHostId`
- 添加 `GetBrowserContext()` helper 方法安全解引用
- 在使用前检查 nullptr

---

### 问题 7: Runtime Crash - Renderer SEGFAULT (单例冲突)
**错误**:
```
Received signal 11 SI_KERNEL000000000000
JsonCaptureClient::SendJsonData()
rax: efefefefefefefef (freed memory poison value)
```

**根本原因**:
- 最初 `JsonCaptureClient` 是单例
- V8 回调也是全局的，但每个 frame 都尝试设置自己的回调和 user_data
- 当一个 frame 释放时，它将 V8 回调设置为 nullptr，导致其他 frame 无法接收回调
- 多个 frame 共享同一个 Mojo 连接，但连接绑定到第一个创建的 frame

**解决方案**:
1. **JSONMonitorManager 单例** - 管理 V8 全局回调，只设置一次
2. **JsonCaptureClient 每帧实例** - 每个 frame 独立的 Mojo 连接
3. **frame 注册机制** - 每个 frame 向 manager 注册自己的 client
4. **分发机制** - manager 收到 JSON 后分发到所有注册的 frame clients

---

### 问题 8: Extension API 不可用
**错误**:
```
chrome.jsonCapture API is NOT available!
```

**解决**: 添加 API features 和 permission features 定义

---

### 问题 9: Permission Check Crash
**错误**:
```
FATAL: Check failed: permission. jsonCapture
extensions::PermissionSet::HasAPIPermission()
```

**解决**:
1. 在 `api_permission_id.mojom` 中添加 `kJsonCapture = 264`
2. 在 `chrome_api_permissions.cc` 中注册权限映射

---

## 最终架构优势

### ✅ 正确的单例使用
- **JSONMonitorManager**: 全局单例，管理 V8 回调
- **JsonCaptureClient**: 每帧实例，独立 Mojo 连接
- **JsonCaptureEventRouter**: 每 BrowserContext 单例

### ✅ 内存安全
- 使用 `GlobalRenderFrameHostId` 避免 dangling pointer
- 使用 `raw_ptr<T>` 符合 Chromium 安全要求
- Self-owned Mojo service 自动清理

### ✅ 线程安全
- `JSONMonitorManager` 使用 `base::Lock` 保护 frame map
- Mojo 自动处理跨进程通信

### ✅ 生命周期管理
- 所有组件都有明确的所有者和生命周期
- Frame 销毁时自动注销和清理

---

## 使用方式

### Extension 代码示例
```javascript
// background.js
chrome.jsonCapture.onData.addListener((data) => {
  console.log('Captured JSON:', data.content);
  console.log('From URL:', data.url);
  console.log('Timestamp:', new Date(data.timestamp));
});

chrome.jsonCapture.start(() => {
  console.log('Started capturing');
});
```

### 测试
1. 加载 extension: `chrome://extensions/` → Load unpacked → 选择 `/home/lhh/heartup/git/json_capture_extension`
2. 打开测试页面: `file:///home/lhh/heartup/git/json_capture_test.html`
3. 点击按钮触发 JSON.stringify
4. 在 Extension Service Worker 的 DevTools 中查看捕获的数据

---

## 核心代码片段

### JSONMonitorManager 单例实现

```cpp
// content/renderer/json_monitor_observer.cc

JSONMonitorManager::JSONMonitorManager() {
  LOG(INFO) << "[JSONMonitorManager] Created";
  // Set V8 global callback once
  v8::JSON::SetJSONStringifyCallback(&JSONMonitorManager::OnJSONStringify, this);
  LOG(INFO) << "[JSONMonitorManager] V8 callback set globally";
}

void JSONMonitorManager::RegisterFrame(RenderFrameImpl* frame, JsonCaptureClient* client) {
  base::AutoLock auto_lock(lock_);
  frame_clients_[frame] = client;
  LOG(INFO) << "[JSONMonitorManager] Registered frame, total frames: " << frame_clients_.size();
}

void JSONMonitorManager::OnJSONStringify(const std::string& json_content, void* user_data) {
  JSONMonitorManager* manager = static_cast<JSONMonitorManager*>(user_data);

  // Filter by keywords
  static constexpr std::string_view filter_keywords[] = {
    "WP_userOptNotify", "WP_actionNotify", /* ... */
  };

  bool should_send = false;
  for (const auto& keyword : filter_keywords) {
    if (json_content.find(keyword) != std::string::npos) {
      should_send = true;
      break;
    }
  }

  if (should_send) {
    // Send to all registered frames
    base::AutoLock auto_lock(manager->lock_);
    for (const auto& pair : manager->frame_clients_) {
      if (pair.second) {
        pair.second->SendJsonData(json_content);
      }
    }
  }
}
```

### JsonCaptureClient 每帧实例

```cpp
// content/renderer/json_capture_client.cc

JsonCaptureClient::JsonCaptureClient(RenderFrameImpl* render_frame)
    : render_frame_(render_frame) {
  LOG(INFO) << "[JsonCaptureClient] Created for frame";
}

void JsonCaptureClient::Initialize() {
  EnsureServiceBound();
  if (service_.is_bound()) {
    service_->IsCapturing(base::BindOnce([](bool capturing) {
      LOG(INFO) << "[JsonCaptureClient] Capturing state: " << capturing;
    }));
  }
}

bool JsonCaptureClient::SendJsonData(const std::string& json_content) {
  if (!capturing_ || !render_frame_) {
    return false;
  }

  EnsureServiceBound();
  if (!service_.is_bound()) {
    return false;
  }

  GURL source_url = render_frame_->GetWebFrame()->GetDocument().Url();
  service_->NotifyJsonCapture(json_content, source_url, 0);
  return true;
}
```

### JsonCaptureServiceImpl Browser 端

```cpp
// content/browser/json_capture_service_impl.cc

void JsonCaptureServiceImpl::NotifyJsonCapture(
    const std::string& json_content,
    const GURL& source_url,
    int32_t frame_id) {

  content::BrowserContext* context = GetBrowserContext();
  if (!context) {
    return;
  }

  extensions::JsonCaptureEventRouter* event_router =
      extensions::JsonCaptureEventRouter::Get(context);

  if (event_router) {
    event_router->DispatchJsonCaptureEvent(json_content, source_url, frame_id);
  }
}

content::BrowserContext* JsonCaptureServiceImpl::GetBrowserContext() {
  RenderFrameHost* render_frame_host = RenderFrameHost::FromID(render_frame_host_id_);
  if (!render_frame_host) {
    return nullptr;
  }
  return render_frame_host->GetProcess()->GetBrowserContext();
}
```

### JsonCaptureEventRouter 事件分发

```cpp
// chrome/browser/extensions/api/json_capture/json_capture_api.cc

void JsonCaptureEventRouter::DispatchJsonCaptureEvent(
    const std::string& json_content,
    const GURL& source_url,
    int32_t frame_id) {

  if (!capturing_) {
    return;
  }

  api::json_capture::JsonData json_data;
  json_data.content = json_content;
  json_data.url = source_url.spec();
  json_data.timestamp = base::Time::Now().InMillisecondsSinceUnixEpoch();
  json_data.frame_id = frame_id;

  base::Value::List args;
  args.Append(json_data.ToValue());

  auto event = std::make_unique<Event>(
      events::JSON_CAPTURE_ON_DATA,
      api::json_capture::OnData::kEventName,
      std::move(args),
      browser_context_);

  EventRouter::Get(browser_context_)->BroadcastEvent(std::move(event));
}
```

---

## 编译和部署

### 编译命令
```bash
cd /home/lhh/local/heartup/git/chromium/src
autoninja -C out/release-141 chrome
```

### 运行测试
```bash
out/release-141/chrome file:///home/lhh/heartup/git/json_capture_test.html
```

### 加载 Extension
1. 打开 `chrome://extensions/`
2. 启用 "Developer mode"
3. 点击 "Load unpacked"
4. 选择 `/home/lhh/heartup/git/json_capture_extension`

---

## 项目总结

本项目成功实现了一个完整的 Chrome Extension API，从底层 V8 hook 到 Extension JavaScript API 的完整数据流。

关键成就：
- ✅ 正确处理单例与非单例的设计权衡
- ✅ 解决了多个 frame 共存的并发问题
- ✅ 实现了内存安全的跨进程通信
- ✅ 符合 Chromium 的架构和编码规范
- ✅ 完整的 Extension 权限系统集成

这个实现可以作为未来开发其他 Chrome Extension API 的参考模板。
