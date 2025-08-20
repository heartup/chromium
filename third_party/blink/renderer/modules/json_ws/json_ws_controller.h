// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_H_

#include "third_party/blink/renderer/bindings/core/v8/script_wrappable.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class ExecutionContext;

// JSONWebSocketController provides JavaScript interface to control 
// the JSON stringify hook and WebSocket functionality.
// Exposed as window.__jsonWsController (only in debug builds)
class MODULES_EXPORT JSONWebSocketController final : public ScriptWrappable {
  DEFINE_WRAPPERTYPEINFO();

 public:
  explicit JSONWebSocketController(ExecutionContext* context);

  // JavaScript interface methods
  void enableHook(const String& websocket_url);
  void disableHook();
  bool isHookEnabled() const;
  String getWebSocketUrl() const;

  void Trace(Visitor* visitor) const override;

 private:
  Member<ExecutionContext> execution_context_;
  String websocket_url_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_H_
