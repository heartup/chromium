// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/json_capture_service_impl.h"

#include "base/logging.h"
#include "chrome/browser/extensions/api/json_capture/json_capture_api.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"

namespace content {

// static
void JsonCaptureServiceImpl::Create(
    RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::JsonCaptureService> receiver) {
  // Self-owned instance that deletes itself on disconnect
  new JsonCaptureServiceImpl(render_frame_host, std::move(receiver));
}

JsonCaptureServiceImpl::JsonCaptureServiceImpl(
    RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<mojom::JsonCaptureService> receiver)
    : render_frame_host_id_(render_frame_host->GetGlobalId()),
      receiver_(this, std::move(receiver)) {
  LOG(INFO) << "[JsonCaptureService] Service created for frame";

  // Set disconnect handler for cleanup
  receiver_.set_disconnect_handler(base::BindOnce(
      [](JsonCaptureServiceImpl* service) {
        LOG(INFO) << "[JsonCaptureService] Service disconnected, self-deleting";
        delete service;
      },
      base::Unretained(this)));
}

JsonCaptureServiceImpl::~JsonCaptureServiceImpl() {
  LOG(INFO) << "[JsonCaptureService] Service destroyed";
}

content::BrowserContext* JsonCaptureServiceImpl::GetBrowserContext() {
  // Safely get the RenderFrameHost using the stored ID
  RenderFrameHost* render_frame_host =
      RenderFrameHost::FromID(render_frame_host_id_);
  if (!render_frame_host) {
    LOG(WARNING) << "[JsonCaptureService] RenderFrameHost no longer exists";
    return nullptr;
  }

  return render_frame_host->GetProcess()->GetBrowserContext();
}

void JsonCaptureServiceImpl::NotifyJsonCapture(
    const std::string& json_content,
    const GURL& source_url,
    int32_t frame_id) {

  // Get BrowserContext safely
  content::BrowserContext* context = GetBrowserContext();
  if (!context) {
    LOG(WARNING) << "[JsonCaptureService] BrowserContext not available";
    return;
  }

  // Dispatch to Extension API
  extensions::JsonCaptureEventRouter* event_router =
      extensions::JsonCaptureEventRouter::Get(context);

  if (event_router) {
    event_router->DispatchJsonCaptureEvent(json_content, source_url, frame_id);
    LOG(INFO) << "[JsonCaptureService] Dispatched event to extensions for URL: "
              << source_url.spec();
  } else {
    LOG(WARNING) << "[JsonCaptureService] Event router not available";
  }
}

void JsonCaptureServiceImpl::StartCapturing(StartCapturingCallback callback) {
  content::BrowserContext* context = GetBrowserContext();
  if (!context) {
    std::move(callback).Run(false);
    return;
  }

  extensions::JsonCaptureEventRouter* event_router =
      extensions::JsonCaptureEventRouter::Get(context);

  if (event_router) {
    event_router->StartCapturing();
    LOG(INFO) << "[JsonCaptureService] Started capturing";
    std::move(callback).Run(true);
  } else {
    LOG(WARNING) << "[JsonCaptureService] Cannot start capturing, router not available";
    std::move(callback).Run(false);
  }
}

void JsonCaptureServiceImpl::StopCapturing(StopCapturingCallback callback) {
  content::BrowserContext* context = GetBrowserContext();
  if (!context) {
    std::move(callback).Run(false);
    return;
  }

  extensions::JsonCaptureEventRouter* event_router =
      extensions::JsonCaptureEventRouter::Get(context);

  if (event_router) {
    event_router->StopCapturing();
    LOG(INFO) << "[JsonCaptureService] Stopped capturing";
    std::move(callback).Run(true);
  } else {
    LOG(WARNING) << "[JsonCaptureService] Cannot stop capturing, router not available";
    std::move(callback).Run(false);
  }
}

void JsonCaptureServiceImpl::IsCapturing(IsCapturingCallback callback) {
  content::BrowserContext* context = GetBrowserContext();
  if (!context) {
    std::move(callback).Run(false);
    return;
  }

  extensions::JsonCaptureEventRouter* event_router =
      extensions::JsonCaptureEventRouter::Get(context);

  bool capturing = event_router ? event_router->IsCapturing() : false;
  std::move(callback).Run(capturing);
}

}  // namespace content
