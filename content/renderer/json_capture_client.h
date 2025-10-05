// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_JSON_CAPTURE_CLIENT_H_
#define CONTENT_RENDERER_JSON_CAPTURE_CLIENT_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "content/common/json_capture.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {

class RenderFrameImpl;

// Client class for communicating with JsonCaptureService in browser process
// This runs in the renderer process and uses Mojo IPC to send JSON data
// Each RenderFrameImpl should have its own instance
class JsonCaptureClient {
 public:
  explicit JsonCaptureClient(RenderFrameImpl* render_frame);
  ~JsonCaptureClient();

  JsonCaptureClient(const JsonCaptureClient&) = delete;
  JsonCaptureClient& operator=(const JsonCaptureClient&) = delete;

  // Initialize the Mojo connection
  void Initialize();

  // Send JSON data to browser process for extension dispatch
  bool SendJsonData(const std::string& json_content);

  // Check if capturing is enabled
  bool IsCapturing();

 private:
  // Ensure service is bound
  void EnsureServiceBound();

  // The RenderFrameImpl that owns this client
  raw_ptr<RenderFrameImpl> render_frame_;

  // Mojo remote to the browser process service
  mojo::Remote<mojom::JsonCaptureService> service_;

  // Cached capturing state
  bool capturing_ = true;
};

}  // namespace content

#endif  // CONTENT_RENDERER_JSON_CAPTURE_CLIENT_H_
