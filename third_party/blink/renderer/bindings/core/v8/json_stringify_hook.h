// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_JSON_STRINGIFY_HOOK_H_
#define THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_JSON_STRINGIFY_HOOK_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "v8/include/v8.h"

namespace blink {

class ExecutionContext;

// JSONStringifyHook provides a centralized point for all JSON.stringify operations
// in Blink. It wraps the standard v8::JSON::Stringify call and optionally sends
// the stringified result through WebSocket if the hook is enabled.
class CORE_EXPORT JSONStringifyHook {
 public:
  // Main hook function that replaces direct calls to v8::JSON::Stringify
  // Parameters match v8::JSON::Stringify exactly
  static v8::MaybeLocal<v8::String> Stringify(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> json_object,
      v8::Local<v8::String> gap = v8::Local<v8::String>());

  // Overload for replacer function support
  static v8::MaybeLocal<v8::String> Stringify(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> json_object,
      v8::Local<v8::Value> replacer,
      v8::Local<v8::String> gap = v8::Local<v8::String>());

  // Enable the hook for a specific execution context with WebSocket URL
  static void EnableHook(ExecutionContext* context, const String& websocket_url);
  
  // Disable the hook for a specific execution context
  static void DisableHook(ExecutionContext* context);
  
  // Check if hook is enabled for the current context
  static bool IsHookEnabled(ExecutionContext* context);

 private:
  // Internal helper to perform the actual stringify and optionally send via WebSocket
  static v8::MaybeLocal<v8::String> StringifyInternal(
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> json_object,
      v8::Local<v8::Value> replacer,
      v8::Local<v8::String> gap);
      
  // Send stringified JSON through WebSocket if enabled
  static void SendViaWebSocket(ExecutionContext* context, const String& json_string);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_JSON_STRINGIFY_HOOK_H_
