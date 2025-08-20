// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/json_ws/json_ws_controller_global_scope.h"

#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/json_ws/json_ws_controller.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {

namespace {

class JSONWebSocketControllerSupplment final
    : public GarbageCollected<JSONWebSocketControllerSupplment>,
      public Supplement<LocalDOMWindow> {
 public:
  static const char kSupplementName[];

  static JSONWebSocketControllerSupplment& From(LocalDOMWindow& window) {
    JSONWebSocketControllerSupplment* supplement =
        Supplement<LocalDOMWindow>::From<JSONWebSocketControllerSupplment>(window);
    if (!supplement) {
      supplement = MakeGarbageCollected<JSONWebSocketControllerSupplment>(window);
      ProvideTo(window, supplement);
    }
    return *supplement;
  }

  explicit JSONWebSocketControllerSupplment(LocalDOMWindow& window)
      : Supplement<LocalDOMWindow>(window),
        controller_(MakeGarbageCollected<JSONWebSocketController>(window.GetExecutionContext())) {}

  JSONWebSocketController* GetController() const { return controller_.Get(); }

  void Trace(Visitor* visitor) const override {
    visitor->Trace(controller_);
    Supplement<LocalDOMWindow>::Trace(visitor);
  }

 private:
  Member<JSONWebSocketController> controller_;
};

const char JSONWebSocketControllerSupplment::kSupplementName[] =
    "JSONWebSocketControllerSupplment";

}  // namespace

// static
JSONWebSocketController* JSONWebSocketControllerGlobalScope::jsonWsController(
    LocalDOMWindow& window) {
  return JSONWebSocketControllerSupplment::From(window).GetController();
}

}  // namespace blink
