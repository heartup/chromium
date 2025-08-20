// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/json_ws/json_ws_controller.h"

#include "third_party/blink/renderer/bindings/core/v8/json_stringify_hook.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/modules/json_ws/json_ws_sender.h"

namespace blink {

JSONWebSocketController::JSONWebSocketController(ExecutionContext* context)
    : execution_context_(context) {}

void JSONWebSocketController::enableHook(const String& websocket_url) {
  if (!execution_context_) {
    return;
  }
  
  websocket_url_ = websocket_url;
  JSONStringifyHook::EnableHook(execution_context_, websocket_url);
}

void JSONWebSocketController::disableHook() {
  if (!execution_context_) {
    return;
  }
  
  JSONStringifyHook::DisableHook(execution_context_);
  websocket_url_ = String();
}

bool JSONWebSocketController::isHookEnabled() const {
  if (!execution_context_) {
    return false;
  }
  
  return JSONStringifyHook::IsHookEnabled(execution_context_);
}

String JSONWebSocketController::getWebSocketUrl() const {
  return websocket_url_;
}

void JSONWebSocketController::Trace(Visitor* visitor) const {
  visitor->Trace(execution_context_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
