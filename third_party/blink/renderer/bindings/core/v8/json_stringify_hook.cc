// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/bindings/core/v8/json_stringify_hook.h"

#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

// Supplement to store sender per ExecutionContext
class JSONWebSocketSenderSupplement final
    : public GarbageCollected<JSONWebSocketSenderSupplement>,
      public Supplement<ExecutionContext> {
 public:
  static const char kSupplementName[];
  
  explicit JSONWebSocketSenderSupplement(ExecutionContext& context)
      : Supplement<ExecutionContext>(context) {}
  
  static JSONWebSocketSenderSupplement& From(ExecutionContext& context) {
    JSONWebSocketSenderSupplement* supplement =
        Supplement<ExecutionContext>::From<JSONWebSocketSenderSupplement>(context);
    if (!supplement) {
      supplement = MakeGarbageCollected<JSONWebSocketSenderSupplement>(context);
      ProvideTo(context, supplement);
    }
    return *supplement;
  }

  JSONWebSocketSenderInterface* GetSender() const { return sender_; }
  
  void SetSender(JSONWebSocketSenderInterface* sender) {
    sender_ = sender;
  }
  
  void Trace(Visitor* visitor) const override {
    // Note: sender_ is not a Member<> because JSONWebSocketSenderInterface
    // is not GarbageCollected. The actual implementation (JSONWebSocketSender)
    // is a Supplement that manages its own lifetime.
    Supplement<ExecutionContext>::Trace(visitor);
  }

 private:
  // Raw pointer is safe because JSONWebSocketSender is a Supplement
  // with the same lifetime as ExecutionContext
  JSONWebSocketSenderInterface* sender_ = nullptr;
};

const char JSONWebSocketSenderSupplement::kSupplementName[] = "JSONWebSocketSenderSupplement";

// Registry implementation
JSONWebSocketSenderRegistry::CreateSenderCallback 
    JSONWebSocketSenderRegistry::sender_factory_ = nullptr;

// static
void JSONWebSocketSenderRegistry::RegisterSenderFactory(CreateSenderCallback callback) {
  sender_factory_ = callback;
}

// static
JSONWebSocketSenderInterface* JSONWebSocketSenderRegistry::CreateSender(ExecutionContext* context) {
  if (sender_factory_ && context) {
    return sender_factory_(context);
  }
  return nullptr;
}

// JSONStringifyHook implementation

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
  
  JSONWebSocketSenderSupplement& supplement = 
      JSONWebSocketSenderSupplement::From(*context);
  
  if (!supplement.GetSender()) {
    // Create sender using registered factory
    JSONWebSocketSenderInterface* sender = 
        JSONWebSocketSenderRegistry::CreateSender(context);
    supplement.SetSender(sender);
  }
  
  if (supplement.GetSender()) {
    supplement.GetSender()->Initialize(websocket_url);
  }
}

// static
void JSONStringifyHook::DisableHook(ExecutionContext* context) {
  if (!context) {
    return;
  }
  
  JSONWebSocketSenderSupplement& supplement = 
      JSONWebSocketSenderSupplement::From(*context);
  
  if (supplement.GetSender()) {
    supplement.GetSender()->Shutdown();
  }
}

// static
bool JSONStringifyHook::IsHookEnabled(ExecutionContext* context) {
  if (!context) {
    return false;
  }
  
  JSONWebSocketSenderSupplement& supplement = 
      JSONWebSocketSenderSupplement::From(*context);
  
  return supplement.GetSender() && supplement.GetSender()->IsEnabled();
}

// static
v8::MaybeLocal<v8::String> JSONStringifyHook::StringifyInternal(
    v8::Local<v8::Context> context,
    v8::Local<v8::Value> json_object,
    v8::Local<v8::Value> replacer,
    v8::Local<v8::String> gap) {
  
  // V8's JSON::Stringify only accepts context and object parameters
  v8::MaybeLocal<v8::String> result = v8::JSON::Stringify(context, json_object);
  
  // If stringify succeeded and hook is enabled, send via WebSocket
  v8::Local<v8::String> result_string;
  if (result.ToLocal(&result_string)) {
    ExecutionContext* execution_context = ExecutionContext::From(context);
    if (execution_context && IsHookEnabled(execution_context)) {
      v8::Isolate* isolate = context->GetIsolate();
      String blink_string = ToCoreString(isolate, result_string);
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
  
  JSONWebSocketSenderSupplement& supplement = 
      JSONWebSocketSenderSupplement::From(*context);
  
  if (supplement.GetSender() && supplement.GetSender()->IsEnabled()) {
    supplement.GetSender()->Send(json_string);
  }
}

}  // namespace blink
