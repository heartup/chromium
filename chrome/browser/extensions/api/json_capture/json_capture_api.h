// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_API_JSON_CAPTURE_JSON_CAPTURE_API_H_
#define CHROME_BROWSER_EXTENSIONS_API_JSON_CAPTURE_JSON_CAPTURE_API_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/common/extensions/api/json_capture.h"
#include "content/public/browser/browser_context.h"
#include "extensions/browser/browser_context_keyed_api_factory.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_function.h"
#include "url/gurl.h"

namespace extensions {

// Event router for JSON capture events
// This is a BrowserContext-keyed service that dispatches JSON capture events
class JsonCaptureEventRouter : public BrowserContextKeyedAPI {
 public:
  explicit JsonCaptureEventRouter(content::BrowserContext* context);
  ~JsonCaptureEventRouter() override;

  JsonCaptureEventRouter(const JsonCaptureEventRouter&) = delete;
  JsonCaptureEventRouter& operator=(const JsonCaptureEventRouter&) = delete;

  static BrowserContextKeyedAPIFactory<JsonCaptureEventRouter>*
  GetFactoryInstance();
  static JsonCaptureEventRouter* Get(content::BrowserContext* context);

  // Dispatch JSON capture event to extensions
  void DispatchJsonCaptureEvent(const std::string& json_content,
                                const GURL& source_url,
                                int32_t frame_id);

  // Control capturing state
  void StartCapturing();
  void StopCapturing();
  bool IsCapturing() const { return capturing_; }

 private:
  friend class BrowserContextKeyedAPIFactory<JsonCaptureEventRouter>;
  static const char* service_name() { return "JsonCaptureEventRouter"; }

  raw_ptr<content::BrowserContext> browser_context_;
  bool capturing_ = true;
};

// Extension function: jsonCapture.start
class JsonCaptureStartFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("jsonCapture.start", JSONCAPTURE_START)

 protected:
  ~JsonCaptureStartFunction() override = default;
  ResponseAction Run() override;
};

// Extension function: jsonCapture.stop
class JsonCaptureStopFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("jsonCapture.stop", JSONCAPTURE_STOP)

 protected:
  ~JsonCaptureStopFunction() override = default;
  ResponseAction Run() override;
};

// Extension function: jsonCapture.isCapturing
class JsonCaptureIsCapturingFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("jsonCapture.isCapturing", JSONCAPTURE_ISCAPTURING)

 protected:
  ~JsonCaptureIsCapturingFunction() override = default;
  ResponseAction Run() override;
};

}  // namespace extensions

#endif  // CHROME_BROWSER_EXTENSIONS_API_JSON_CAPTURE_JSON_CAPTURE_API_H_
