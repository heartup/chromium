// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_SENDER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_SENDER_H_

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/websocket.mojom-blink.h"
#include "third_party/blink/public/mojom/websockets/websocket_connector.mojom-blink.h"
#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_receiver.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_remote.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class ExecutionContext;
class KURL;

// JSONWebSocketSender manages a WebSocket connection for sending JSON data.
// It's implemented as a Supplement to ExecutionContext, so each context
// can have its own sender instance.
class MODULES_EXPORT JSONWebSocketSender final
    : public GarbageCollected<JSONWebSocketSender>,
      public Supplement<ExecutionContext>,
      public ExecutionContextLifecycleObserver,
      public network::mojom::blink::WebSocketHandshakeClient {
 public:
  static const char kSupplementName[];
  
  // Get or create the JSONWebSocketSender for the given ExecutionContext
  static JSONWebSocketSender* From(ExecutionContext* context);

  explicit JSONWebSocketSender(ExecutionContext& context);
  ~JSONWebSocketSender() override;

  // Initialize WebSocket connection to the specified URL
  void Initialize(const String& websocket_url);

  // Send JSON string through the WebSocket connection
  void Send(const String& json_data);

  // Check if the sender is enabled and connected
  bool IsEnabled() const;

  // Shutdown the WebSocket connection
  void Shutdown();

  // GarbageCollected implementation
  void Trace(Visitor* visitor) const override;

  // ExecutionContextLifecycleObserver implementation
  void ContextDestroyed() override;

  // WebSocketHandshakeClient implementation
  void OnOpeningHandshakeStarted(
      network::mojom::blink::WebSocketHandshakeRequestPtr request) override;
      
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::blink::WebSocket> websocket,
      mojo::PendingReceiver<network::mojom::blink::WebSocketClient> client_receiver,
      network::mojom::blink::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  void OnFailure(const String& message,
                 int32_t net_error,
                 int32_t response_code) override;

 private:
  enum class State {
    kDisconnected,
    kConnecting,
    kConnected,
    kError
  };

  void ConnectWebSocket(const KURL& url);
  void OnWebSocketError();
  void SendDataThroughPipe(const String& data);

  State state_ = State::kDisconnected;
  String websocket_url_;
  
  HeapMojoRemote<network::mojom::blink::WebSocket> websocket_;
  HeapMojoReceiver<network::mojom::blink::WebSocketHandshakeClient, JSONWebSocketSender> handshake_receiver_;
  
  mojo::ScopedDataPipeProducerHandle data_pipe_producer_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_JSON_WS_JSON_WS_SENDER_H_
