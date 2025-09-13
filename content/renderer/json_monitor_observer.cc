#include "content/renderer/json_monitor_observer.h"
#include "content/renderer/render_frame_impl.h"
#include "content/public/common/isolated_world_ids.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "content/renderer/websocket_client.h"
#include <memory>
#include <cstdio>

namespace content {

void JSONMonitor::Initialize(RenderFrameImpl* render_frame) {
    // 设置V8回调
    v8::Isolate* isolate = render_frame->GetWebFrame()->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope handle_scope(isolate);

    // 调用V8的SetJSONStringifyCallback函数
    v8::JSON::SetJSONStringifyCallback(&JSONMonitor::OnJSONStringify, render_frame);

    // 使用全局函数初始化WebSocket连接
    printf("[JSONMonitor] Attempting to initialize WebSocket connection to 127.0.0.1:8080\n");
    bool connected = blink::internal::InitializeWebSocketClient("127.0.0.1", 8080, "/");
    if (connected) {
        printf("[JSONMonitor] WebSocket connection established or already connected\n");
    } else {
        printf("[JSONMonitor] Failed to establish WebSocket connection\n");
    }
}

void JSONMonitor::OnJSONStringify(const std::string& json_content, void* user_data) {
    // 使用全局函数发送JSON到WebSocket
    blink::internal::SendJsonToWebSocket(json_content);
    printf("json_content: %s\n", json_content.c_str());
}

JSONMonitorObserver::JSONMonitorObserver(RenderFrame* render_frame)
    : RenderFrameObserver(render_frame) {
}

JSONMonitorObserver::~JSONMonitorObserver() = default;

void JSONMonitorObserver::OnDestruct() {
  delete this;
}

void JSONMonitorObserver::DidCreateScriptContext(v8::Local<v8::Context> context,
                                                 int world_id) {
  // 只在主世界中初始化一次
  if (world_id == ISOLATED_WORLD_ID_GLOBAL && !initialized_) {
    printf("[JSONMonitorObserver] Creating script context for new tab/frame\n");
    json_monitor_ = std::make_unique<JSONMonitor>();
    json_monitor_->Initialize(static_cast<RenderFrameImpl*>(render_frame()));
    initialized_ = true;
    printf("[JSONMonitorObserver] Initialization complete for tab/frame\n");
  }
}

void JSONMonitorObserver::WillReleaseScriptContext(v8::Local<v8::Context> context,
                                                   int world_id) {
  if (world_id == ISOLATED_WORLD_ID_GLOBAL && initialized_) {
    printf("[JSONMonitorObserver] Releasing script context for tab/frame\n");
    // 注意：不清理全局WebSocket连接，因为其他标签页可能还在使用
    // 只清理本地的 json_monitor 实例
    json_monitor_.reset();
    initialized_ = false;
  }
}

}  // namespace content