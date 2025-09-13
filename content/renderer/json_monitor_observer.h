#ifndef CONTENT_RENDERER_JSON_MONITOR_OBSERVER_H_
#define CONTENT_RENDERER_JSON_MONITOR_OBSERVER_H_

#include "content/public/renderer/render_frame_observer.h"
#include "content/renderer/render_frame_impl.h"

#include "v8/include/v8-context.h"
#include <string>
#include <memory>

namespace content {

class JSONMonitor {
  public:
    JSONMonitor() = default;
    ~JSONMonitor() = default;
    
    void Initialize(RenderFrameImpl* render_frame);
    static void OnJSONStringify(const std::string& json_content, void* user_data);
    
  private:
    // 移除WebSocket客户端管理，使用全局函数
};

class JSONMonitorObserver : public RenderFrameObserver {
public:
  explicit JSONMonitorObserver(RenderFrame* render_frame);
  ~JSONMonitorObserver() override;

  // RenderFrameObserver implementation
  void OnDestruct() override;
  void DidCreateScriptContext(v8::Local<v8::Context> context, int world_id) override;
  void WillReleaseScriptContext(v8::Local<v8::Context> context, int world_id) override;

private:
  std::unique_ptr<JSONMonitor> json_monitor_;
  bool initialized_ = false;
};

}  // namespace content

#endif  // CONTENT_RENDERER_JSON_MONITOR_OBSERVER_H_