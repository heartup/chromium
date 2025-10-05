#ifndef CONTENT_RENDERER_JSON_MONITOR_OBSERVER_H_
#define CONTENT_RENDERER_JSON_MONITOR_OBSERVER_H_

#include "content/public/renderer/render_frame_observer.h"
#include "content/renderer/render_frame_impl.h"

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "v8/include/v8-context.h"
#include <string>
#include <memory>
#include <map>

namespace content {

class JsonCaptureClient;

// Global manager for all JSON monitors across frames
class JSONMonitorManager {
 public:
  static JSONMonitorManager* GetInstance();

  JSONMonitorManager();
  ~JSONMonitorManager();

  JSONMonitorManager(const JSONMonitorManager&) = delete;
  JSONMonitorManager& operator=(const JSONMonitorManager&) = delete;

  void RegisterFrame(RenderFrameImpl* frame, JsonCaptureClient* client);
  void UnregisterFrame(RenderFrameImpl* frame);

  // Global callback for V8
  static void OnJSONStringify(const std::string& json_content, void* user_data);

 private:
  base::Lock lock_;
  std::map<RenderFrameImpl*, JsonCaptureClient*> frame_clients_;
};

class JSONMonitor {
  public:
    explicit JSONMonitor(RenderFrameImpl* render_frame);
    ~JSONMonitor();

    void Initialize();

  private:
    raw_ptr<RenderFrameImpl> render_frame_;
    std::unique_ptr<JsonCaptureClient> capture_client_;
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