// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_JSON_CAPTURE_SERVICE_IMPL_H_
#define CONTENT_BROWSER_JSON_CAPTURE_SERVICE_IMPL_H_

#include "content/common/json_capture.mojom.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace content {

// Browser-side implementation of JsonCaptureService
// Receives JSON data from renderer and dispatches to extension API
// Self-owned - deletes itself when the mojo connection is closed
class JsonCaptureServiceImpl : public mojom::JsonCaptureService {
 public:
  static void Create(
      RenderFrameHost* render_frame_host,
      mojo::PendingReceiver<mojom::JsonCaptureService> receiver);

  JsonCaptureServiceImpl(
      RenderFrameHost* render_frame_host,
      mojo::PendingReceiver<mojom::JsonCaptureService> receiver);
  ~JsonCaptureServiceImpl() override;

  JsonCaptureServiceImpl(const JsonCaptureServiceImpl&) = delete;
  JsonCaptureServiceImpl& operator=(const JsonCaptureServiceImpl&) = delete;

  // mojom::JsonCaptureService implementation
  void NotifyJsonCapture(const std::string& json_content,
                        const GURL& source_url,
                        int32_t frame_id) override;
  void StartCapturing(StartCapturingCallback callback) override;
  void StopCapturing(StopCapturingCallback callback) override;
  void IsCapturing(IsCapturingCallback callback) override;

 private:
  // Helper to get BrowserContext safely
  content::BrowserContext* GetBrowserContext();

  // Store the GlobalRenderFrameHostId instead of raw pointer
  const GlobalRenderFrameHostId render_frame_host_id_;
  mojo::Receiver<mojom::JsonCaptureService> receiver_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_JSON_CAPTURE_SERVICE_IMPL_H_
