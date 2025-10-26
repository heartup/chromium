// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_V2_H_
#define CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_V2_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "content/browser/network_websocket_manager.h"
#include "content/common/json_websocket.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace network {
namespace mojom {
class NetworkContext;
}
}  // namespace network

namespace content {

// Enhanced implementation of JsonWebSocketService using Chrome's network service
// This version provides better integration with Chrome's lifecycle and network stack
class JsonWebSocketServiceImplV2 : public mojom::JsonWebSocketService {
 public:
  // Factory method to create service with network context
  static void Create(network::mojom::NetworkContext* network_context,
                    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver);

  // Factory method with window ID
  static void CreateWithWindowId(int32_t window_id,
                                 network::mojom::NetworkContext* network_context,
                                 mojo::PendingReceiver<mojom::JsonWebSocketService> receiver);

  JsonWebSocketServiceImplV2(
      network::mojom::NetworkContext* network_context,
      mojo::PendingReceiver<mojom::JsonWebSocketService> receiver,
      int32_t window_id = -1);
  ~JsonWebSocketServiceImplV2() override;

  // mojom::JsonWebSocketService implementation
  void Connect(const std::string& host,
               uint32_t port,
               const std::string& path,
               ConnectCallback callback) override;
  void SendJsonMessage(const std::string& json_message,
                      SendJsonMessageCallback callback) override;
  void Disconnect() override;
  void IsConnected(IsConnectedCallback callback) override;

 private:
  // Handle connection result
  void OnConnectionResult(ConnectCallback callback, bool success);

  // Handle message send result
  void OnMessageSendResult(SendJsonMessageCallback callback, bool success);

  // Network context from browser process
  raw_ptr<network::mojom::NetworkContext> network_context_;

  // Mojo receiver for this service
  mojo::Receiver<mojom::JsonWebSocketService> receiver_;

  // WebSocket manager using network service
  std::unique_ptr<NetworkWebSocketManager> websocket_manager_;

  // Window ID to identify the source window/tab
  int32_t window_id_;

  base::WeakPtrFactory<JsonWebSocketServiceImplV2> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_V2_H_