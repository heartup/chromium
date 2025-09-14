#include "content/renderer/json_monitor_observer.h"
#include "content/renderer/render_frame_impl.h"
#include "content/public/common/isolated_world_ids.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "content/renderer/json_websocket_client.h"
#include "base/logging.h"
#include <memory>
#include <cstdio>
#include <string>
#include <string_view>

namespace content {

void JSONMonitor::Initialize(RenderFrameImpl* render_frame) {
    // 设置V8回调
    v8::Isolate* isolate = render_frame->GetWebFrame()->GetAgentGroupScheduler()->Isolate();
    v8::HandleScope handle_scope(isolate);

    // 调用V8的SetJSONStringifyCallback函数
    v8::JSON::SetJSONStringifyCallback(&JSONMonitor::OnJSONStringify, render_frame);

    // 使用 Mojo IPC 初始化 WebSocket 连接（通过浏览器进程，绕过沙盒）
    LOG(INFO) << "[JSONMonitor] Initializing WebSocket via Mojo IPC";

    bool connected = InitializeJsonWebSocketClient(render_frame);
    if (connected) {
        LOG(INFO) << "[JSONMonitor] WebSocket connection established via Mojo IPC (works in sandbox!)";
    } else {
        LOG(WARNING) << "[JSONMonitor] WebSocket connection via IPC failed";
    }
}

void JSONMonitor::OnJSONStringify(const std::string& json_content, void* user_data) {
    // 使用string_view数组来避免安全警告
    static constexpr std::string_view filter_keywords[] = {
        "WP_userOptNotify",
        "WP_actionNotify",
        "WP_roundChangeNotify",
        "WP_bankerChangeNotify",
        "WP_squidGameNotify",
        "WP_dealNotify",
        "C_updateRoomNotify",
        "C_cleanNotify",
        "WP_playResultNotify"
    };

    // 检查字符串是否包含任何关键字
    bool should_send = false;
    std::string found_keyword;
    for (const auto& keyword : filter_keywords) {
        if (json_content.find(keyword) != std::string::npos) {
            should_send = true;
            found_keyword = std::string(keyword);
            break;
        }
    }

    // 只有包含关键字时才发送
    if (should_send) {
        // 输出特殊格式的日志，便于调试和外部脚本处理
        LOG(INFO) << "[JSON_MONITOR_DATA_START]" << json_content << "[JSON_MONITOR_DATA_END]";

        // 通过 Mojo IPC 发送到浏览器进程，再由浏览器进程发送到 WebSocket 服务器
        if (!SendJsonViaIPC(json_content)) {
            LOG(WARNING) << "[JSONMonitor] IPC send failed";
        } else {
            LOG(INFO) << "[JSONMonitor] JSON sent successfully via Mojo IPC";
        }

        // 同时输出到 stdout 用于调试
        fprintf(stdout, "[JSONMonitor] Found keyword: %s\n", found_keyword.c_str());
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