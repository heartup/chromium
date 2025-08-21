#include "content/renderer/json_monitor_observer.h"
#include "content/renderer/render_frame_impl.h"
#include "content/public/common/isolated_world_ids.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "v8/include/v8-json.h"

namespace content {

void JSONMonitor::Initialize(RenderFrameImpl* render_frame) {
    // 设置V8回调
    v8::Isolate* isolate = render_frame->GetWebFrame()->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope handle_scope(isolate);
    
    // 调用V8的SetJSONStringifyCallback函数
    v8::JSON::SetJSONStringifyCallback(&JSONMonitor::OnJSONStringify, render_frame);
    
    // 初始化WebSocket连接
    websocket_client_ = std::make_unique<blink::internal::WebSocketClient>();
    websocket_client_->Connect("127.0.0.1", 8080, "/");
}
    
void JSONMonitor::OnJSONStringify(const std::string& json_content, void* user_data) {
    // 在Blink线程中处理WebSocket发送
    RenderFrameImpl* render_frame = static_cast<RenderFrameImpl*>(user_data);
    render_frame->GetTaskRunner(blink::TaskType::kInternalTest)->PostTask(
        FROM_HERE, base::BindOnce(&JSONMonitor::SendToWebSocket, json_content));
}
    
void JSONMonitor::SendToWebSocket(const std::string& json_content) {
    if (websocket_client_ && websocket_client_->IsConnected()) {
        websocket_client_->SendMessage(json_content);
    }
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
    json_monitor_ = std::make_unique<JSONMonitor>();
    json_monitor_->Initialize(static_cast<RenderFrameImpl*>(render_frame()));
    initialized_ = true;
  }
}

void JSONMonitorObserver::WillReleaseScriptContext(v8::Local<v8::Context> context, 
                                                   int world_id) {
  if (world_id == ISOLATED_WORLD_ID_GLOBAL && initialized_) {
    json_monitor_.reset();
    initialized_ = false;
  }
}

}  // namespace content