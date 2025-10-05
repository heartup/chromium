// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/json_capture/json_capture_api.h"

#include "base/time/time.h"
#include "base/values.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_event_histogram_value.h"
#include "extensions/common/extension_id.h"

namespace extensions {

// JsonCaptureEventRouter implementation

JsonCaptureEventRouter::JsonCaptureEventRouter(content::BrowserContext* context)
    : browser_context_(context) {
  LOG(INFO) << "[JsonCaptureEventRouter] Created for browser context";
}

JsonCaptureEventRouter::~JsonCaptureEventRouter() {
  LOG(INFO) << "[JsonCaptureEventRouter] Destroyed";
}

// static
BrowserContextKeyedAPIFactory<JsonCaptureEventRouter>*
JsonCaptureEventRouter::GetFactoryInstance() {
  static base::NoDestructor<
      BrowserContextKeyedAPIFactory<JsonCaptureEventRouter>>
      instance;
  return instance.get();
}

// static
JsonCaptureEventRouter* JsonCaptureEventRouter::Get(
    content::BrowserContext* context) {
  return BrowserContextKeyedAPIFactory<JsonCaptureEventRouter>::Get(context);
}

void JsonCaptureEventRouter::DispatchJsonCaptureEvent(
    const std::string& json_content,
    const GURL& source_url,
    int32_t frame_id) {

  if (!capturing_) {
    return;
  }

  // Construct event data
  api::json_capture::JsonData json_data;
  json_data.content = json_content;
  json_data.url = source_url.spec();
  json_data.timestamp = base::Time::Now().InMillisecondsSinceUnixEpoch();
  json_data.frame_id = frame_id;

  // Create event arguments
  base::Value::List args;
  args.Append(json_data.ToValue());

  // Create event
  auto event = std::make_unique<Event>(
      events::JSON_CAPTURE_ON_DATA,
      api::json_capture::OnData::kEventName,
      std::move(args),
      browser_context_);

  // Dispatch event to all listening extensions
  EventRouter::Get(browser_context_)->BroadcastEvent(std::move(event));

  LOG(INFO) << "[JsonCaptureEventRouter] Event dispatched for URL: "
            << source_url.spec();
}

void JsonCaptureEventRouter::StartCapturing() {
  capturing_ = true;
  LOG(INFO) << "[JsonCaptureEventRouter] Capturing started";
}

void JsonCaptureEventRouter::StopCapturing() {
  capturing_ = false;
  LOG(INFO) << "[JsonCaptureEventRouter] Capturing stopped";
}

// Extension function implementations

ExtensionFunction::ResponseAction JsonCaptureStartFunction::Run() {
  JsonCaptureEventRouter::Get(browser_context())->StartCapturing();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction JsonCaptureStopFunction::Run() {
  JsonCaptureEventRouter::Get(browser_context())->StopCapturing();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction JsonCaptureIsCapturingFunction::Run() {
  bool capturing =
      JsonCaptureEventRouter::Get(browser_context())->IsCapturing();
  return RespondNow(WithArguments(capturing));
}

}  // namespace extensions
