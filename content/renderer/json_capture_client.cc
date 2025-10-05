// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/json_capture_client.h"

#include "base/logging.h"
#include "content/public/renderer/render_frame.h"
#include "content/renderer/render_frame_impl.h"
#include "third_party/blink/public/web/web_local_frame.h"

namespace content {

JsonCaptureClient::JsonCaptureClient(RenderFrameImpl* render_frame)
    : render_frame_(render_frame) {
  LOG(INFO) << "[JsonCaptureClient] Created for frame";
}

JsonCaptureClient::~JsonCaptureClient() {
  LOG(INFO) << "[JsonCaptureClient] Destroyed";
}

void JsonCaptureClient::Initialize() {
  EnsureServiceBound();

  // Query current capturing state
  if (service_.is_bound()) {
    service_->IsCapturing(base::BindOnce([](bool capturing) {
      LOG(INFO) << "[JsonCaptureClient] Capturing state: " << capturing;
    }));
  }
}

void JsonCaptureClient::EnsureServiceBound() {
  if (service_.is_bound() || !render_frame_) {
    return;
  }

  render_frame_->GetBrowserInterfaceBroker().GetInterface(
      service_.BindNewPipeAndPassReceiver());

  service_.set_disconnect_handler(base::BindOnce([]() {
    LOG(WARNING) << "[JsonCaptureClient] Service disconnected";
  }));
}

bool JsonCaptureClient::SendJsonData(const std::string& json_content) {
  if (!capturing_) {
    LOG(WARNING) << "[JsonCaptureClient] Not capturing";
    return false;
  }

  if (!render_frame_) {
    LOG(ERROR) << "[JsonCaptureClient] render_frame_ is null";
    return false;
  }

  blink::WebLocalFrame* web_frame = render_frame_->GetWebFrame();
  if (!web_frame) {
    LOG(ERROR) << "[JsonCaptureClient] WebFrame is null";
    return false;
  }

  EnsureServiceBound();

  if (!service_.is_bound()) {
    LOG(ERROR) << "[JsonCaptureClient] Service not bound";
    return false;
  }

  // Get current page URL
  GURL source_url = web_frame->GetDocument().Url();
  // Use 0 as frame_id placeholder - the browser can identify the frame from the Mojo connection
  int32_t frame_id = 0;

  // Send to Browser Process
  service_->NotifyJsonCapture(json_content, source_url, frame_id);

  return true;
}

bool JsonCaptureClient::IsCapturing() {
  return capturing_;
}

}  // namespace content
