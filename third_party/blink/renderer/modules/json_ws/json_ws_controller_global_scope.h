// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_GLOBAL_SCOPE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_GLOBAL_SCOPE_H_

#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

class JSONWebSocketController;
class LocalDOMWindow;

class MODULES_EXPORT JSONWebSocketControllerGlobalScope {
  STATIC_ONLY(JSONWebSocketControllerGlobalScope);

 public:
  static JSONWebSocketController* jsonWsController(LocalDOMWindow&);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_CONTROLLER_GLOBAL_SCOPE_H_
