// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_NETWORK_WEBSOCKET_MANAGER_H_
#define CONTENT_BROWSER_NETWORK_WEBSOCKET_MANAGER_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_ANDROID)
#include "base/android/application_status_listener.h"
#endif

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace content {

// NetworkWebSocketManager manages WebSocket connections using Chrome's
// network service infrastructure. This provides better integration with
// Chrome's lifecycle management and network stack.
class CONTENT_EXPORT NetworkWebSocketManager
    : public network::mojom::WebSocketHandshakeClient,
      public network::mojom::WebSocketClient {
 public:
  // Callback for connection status changes
  using ConnectionCallback = base::OnceCallback<void(bool success)>;
  using MessageCallback = base::OnceCallback<void(bool success)>;

  explicit NetworkWebSocketManager(
      network::mojom::NetworkContext* network_context);
  ~NetworkWebSocketManager() override;

  // Connect to WebSocket server
  void Connect(const std::string& host,
               uint32_t port,
               const std::string& path,
               ConnectionCallback callback);

  // Send message through WebSocket
  void SendMessage(const std::string& message, MessageCallback callback);

  // Disconnect WebSocket
  void Disconnect();

  // Check connection status
  bool IsConnected() const { return connected_; }

  // Application state handling (Android)
  void OnApplicationStateChange(bool is_foreground);

  // network::mojom::WebSocketHandshakeClient implementation
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr request) override;
  void OnFailure(const std::string& message,
                 int32_t net_error,
                 int32_t response_code) override;
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> socket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
      network::mojom::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  // network::mojom::WebSocketClient implementation
  void OnDataFrame(bool fin,
                   network::mojom::WebSocketMessageType type,
                   uint64_t data_length) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const std::string& reason) override;
  void OnClosingHandshake() override;

 private:
  // Android application state callback
#if BUILDFLAG(IS_ANDROID)
  void OnApplicationStateChangeCallback(base::android::ApplicationState state);
#endif

  // Heartbeat mechanism
  void StartHeartbeat();
  void StopHeartbeat();
  void SendPing();
  void OnPingTimeout();
  void HandleReconnect();

  // Authentication mechanism (preserve existing auth)
  bool PerformAuthentication();
  std::string GetAuthKey();
  std::string EncryptKey(const std::string& key);

  // Data pipe handling
  void ReadFromDataPipe();
  void WriteToDataPipe(const std::string& data);
  void OnDataPipeWritable(MojoResult result);
  void OnDataPipeReadable(MojoResult result);

  // Process received message
  void ProcessReceivedMessage(const std::string& message);

  // Network context from Chrome
  raw_ptr<network::mojom::NetworkContext> network_context_;

  // WebSocket connection
  mojo::Remote<network::mojom::WebSocket> websocket_;
  mojo::Receiver<network::mojom::WebSocketHandshakeClient> handshake_receiver_{this};
  mojo::Receiver<network::mojom::WebSocketClient> client_receiver_{this};

  // Data pipes for WebSocket communication
  mojo::ScopedDataPipeConsumerHandle readable_pipe_;
  mojo::ScopedDataPipeProducerHandle writable_pipe_;
  mojo::SimpleWatcher readable_watcher_;
  mojo::SimpleWatcher writable_watcher_;

  // Connection state
  bool connected_ = false;
  bool connecting_ = false;
  bool authenticated_ = false;
  GURL current_url_;

  // Callbacks
  ConnectionCallback connection_callback_;
  MessageCallback message_callback_;

  // Heartbeat mechanism (kept for interface compatibility, but unused)
  // Chrome's WebSocket layer handles Ping/Pong automatically

  // Application state (Android)
  bool app_in_foreground_ = true;
  base::TimeTicks background_time_;

#if BUILDFLAG(IS_ANDROID)
  std::unique_ptr<base::android::ApplicationStatusListener> app_status_listener_;
#endif

  // Message buffer for partial messages
  std::string message_buffer_;

  // Authentication state
  bool waiting_for_auth_ = false;
  std::string expected_auth_key_;

  base::WeakPtrFactory<NetworkWebSocketManager> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_NETWORK_WEBSOCKET_MANAGER_H_