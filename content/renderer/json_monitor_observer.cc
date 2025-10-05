#include "content/renderer/json_monitor_observer.h"
#include "content/renderer/render_frame_impl.h"
#include "content/public/common/isolated_world_ids.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "content/renderer/json_capture_client.h"
#include "base/logging.h"
#include "base/memory/singleton.h"
#include <memory>
#include <cstdio>
#include <string>
#include <string_view>

namespace content {

// JSONMonitorManager implementation

// static
JSONMonitorManager* JSONMonitorManager::GetInstance() {
  return base::Singleton<JSONMonitorManager>::get();
}

JSONMonitorManager::JSONMonitorManager() {
  LOG(INFO) << "[JSONMonitorManager] Created";
  // Set V8 global callback once
  v8::JSON::SetJSONStringifyCallback(&JSONMonitorManager::OnJSONStringify, this);
  LOG(INFO) << "[JSONMonitorManager] V8 callback set globally";
}

JSONMonitorManager::~JSONMonitorManager() {
  LOG(INFO) << "[JSONMonitorManager] Destroyed";
}

void JSONMonitorManager::RegisterFrame(RenderFrameImpl* frame, JsonCaptureClient* client) {
  base::AutoLock auto_lock(lock_);
  frame_clients_[frame] = client;
  LOG(INFO) << "[JSONMonitorManager] Registered frame, total frames: " << frame_clients_.size();
}

void JSONMonitorManager::UnregisterFrame(RenderFrameImpl* frame) {
  base::AutoLock auto_lock(lock_);
  frame_clients_.erase(frame);
  LOG(INFO) << "[JSONMonitorManager] Unregistered frame, total frames: " << frame_clients_.size();
}

// static
void JSONMonitorManager::OnJSONStringify(const std::string& json_content, void* user_data) {
    JSONMonitorManager* manager = static_cast<JSONMonitorManager*>(user_data);
    if (!manager) {
        LOG(WARNING) << "[JSONMonitorManager] Invalid manager";
        return;
    }

    // 记录所有 JSON.stringify 调用
    LOG(INFO) << "[JSONMonitorManager] JSON.stringify called, length: " << json_content.length();

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
        LOG(INFO) << "[JSON_MONITOR_DATA_START]" << found_keyword << "[JSON_MONITOR_DATA_END]";

        // Send to all registered frames
        base::AutoLock auto_lock(manager->lock_);
        LOG(INFO) << "[JSONMonitorManager] Sending to " << manager->frame_clients_.size() << " frames";

        for (const auto& pair : manager->frame_clients_) {
            if (pair.second && pair.second->SendJsonData(json_content)) {
                LOG(INFO) << "[JSONMonitorManager] JSON sent successfully via frame";
            } else {
                LOG(WARNING) << "[JSONMonitorManager] Failed to send JSON via frame";
            }
        }
    } else {
        LOG(INFO) << "[JSONMonitorManager] JSON doesn't match filter, first 100 chars: "
                  << json_content.substr(0, std::min(size_t(100), json_content.length()));
    }
}

// JSONMonitor implementation

JSONMonitor::JSONMonitor(RenderFrameImpl* render_frame)
    : render_frame_(render_frame),
      capture_client_(std::make_unique<JsonCaptureClient>(render_frame)) {
  LOG(INFO) << "[JSONMonitor] Created for frame";
}

JSONMonitor::~JSONMonitor() {
  LOG(INFO) << "[JSONMonitor] Destroyed";
  if (render_frame_) {
    JSONMonitorManager::GetInstance()->UnregisterFrame(render_frame_);
  }
}

void JSONMonitor::Initialize() {
    LOG(INFO) << "[JSONMonitor] Initializing for frame";

    // 使用 Mojo IPC 初始化 JSON Capture 客户端（通过浏览器进程转发到 Extension）
    capture_client_->Initialize();
    LOG(INFO) << "[JSONMonitor] JSON Capture client initialized";

    // Ensure global manager is initialized (this will create it if needed)
    JSONMonitorManager* manager = JSONMonitorManager::GetInstance();
    LOG(INFO) << "[JSONMonitor] Got manager instance: " << manager;

    // Register with global manager
    manager->RegisterFrame(render_frame_, capture_client_.get());
    LOG(INFO) << "[JSONMonitor] Registered with global manager";
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
    RenderFrameImpl* render_frame_impl = static_cast<RenderFrameImpl*>(render_frame());
    json_monitor_ = std::make_unique<JSONMonitor>(render_frame_impl);
    json_monitor_->Initialize();
    initialized_ = true;
    printf("[JSONMonitorObserver] Initialization complete for tab/frame\n");
  }
}

void JSONMonitorObserver::WillReleaseScriptContext(v8::Local<v8::Context> context,
                                                   int world_id) {
  if (world_id == ISOLATED_WORLD_ID_GLOBAL && initialized_) {
    printf("[JSONMonitorObserver] Releasing script context for tab/frame\n");

    // 不需要清除 V8 回调，因为它是全局的，由 JSONMonitorManager 管理
    // 只需要销毁 json_monitor，它会在析构函数中从 manager 注销
    json_monitor_.reset();
    initialized_ = false;
  }
}

}  // namespace content
