// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/bindings/core/v8/json_stringify_hook.h"

#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/json_ws/json_ws_sender.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

// static
v8::MaybeLocal<v8::String> JSONStringifyHook::Stringify(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> json_object,
    v8::Local<v8::String> gap) {
  return StringifyInternal(context, json_object, v8::Local<v8::Value>(), gap);
}

// static
v8::MaybeLocal<v8::String> JSONStringifyHook::Stringify(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> json_object,
    v8::Local<v8::Value> replacer,
    v8::Local<v8::String> gap) {
  return StringifyInternal(context, json_object, replacer, gap);
}

// static
void JSONStringifyHook::EnableHook(ExecutionContext* context, const String& websocket_url) {
  if (!context) {
    return;
  }
  
  JSONWebSocketSender::From(context)->Initialize(websocket_url);
}

// static
void JSONStringifyHook::DisableHook(ExecutionContext* context) {
  if (!context) {
    return;
  }
  
  JSONWebSocketSender::From(context)->Shutdown();
}

// static
bool JSONStringifyHook::IsHookEnabled(ExecutionContext* context) {
  if (!context) {
    return false;
  }
  
  return JSONWebSocketSender::From(context)->IsEnabled();
}

// static
v8::MaybeLocal<v8::String> JSONStringifyHook::StringifyInternal(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> json_object,
    v8::Local<v8::Value> replacer,
    v8::Local<v8::String> gap) {
  
  // Call the standard V8 JSON.stringify
  v8::MaybeLocal<v8::String> result;
  if (replacer.IsEmpty()) {
    result = v8::JSON::Stringify(context, json_object, gap);
  } else {
    result = v8::JSON::Stringify(context, json_object, replacer, gap);
  }
  
  // If stringify succeeded and hook is enabled, send via WebSocket
  v8::Local<v8::String> result_string;
  if (result.ToLocal(&result_string)) {
    ExecutionContext* execution_context = ExecutionContext::From(context);
    if (execution_context && IsHookEnabled(execution_context)) {
      String blink_string = ToCoreString(result_string);
      SendViaWebSocket(execution_context, blink_string);
    }
  }
  
  return result;
}

// static
void JSONStringifyHook::SendViaWebSocket(ExecutionContext* context, const String& json_string) {
  if (!context) {
    return;
  }
  
  JSONWebSocketSender* sender = JSONWebSocketSender::From(context);
  if (sender && sender->IsEnabled()) {
    sender->Send(json_string);
  }
}

}  // namespace blink
