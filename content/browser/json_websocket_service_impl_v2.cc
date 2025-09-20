// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/json_websocket_service_impl_v2.h"

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"

namespace content {

// static
void JsonWebSocketServiceImplV2::Create(
    network::mojom::NetworkContext* network_context,
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver) {
  // Create self-managed instance
  new JsonWebSocketServiceImplV2(network_context, std::move(receiver));
}

JsonWebSocketServiceImplV2::JsonWebSocketServiceImplV2(
    network::mojom::NetworkContext* network_context,
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver)
    : network_context_(network_context),
      receiver_(this, std::move(receiver)) {
  LOG(INFO) << "[JsonWebSocketV2] Service created with network context";

  // Create WebSocket manager
  websocket_manager_ = std::make_unique<NetworkWebSocketManager>(network_context_);

  // Set disconnect handler for cleanup
  receiver_.set_disconnect_handler(
      base::BindOnce([](JsonWebSocketServiceImplV2* service) {
        delete service;
      }, base::Unretained(this)));
}

JsonWebSocketServiceImplV2::~JsonWebSocketServiceImplV2() {
  LOG(INFO) << "[JsonWebSocketV2] Service destroyed";
  if (websocket_manager_) {
    websocket_manager_->Disconnect();
  }
}

void JsonWebSocketServiceImplV2::Connect(const std::string& host,
                                         uint32_t port,
                                         const std::string& path,
                                         ConnectCallback callback) {
  // Check for command line override of port
  uint32_t actual_port = port;
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kJsonWebSocketPort)) {
    std::string port_str = command_line->GetSwitchValueASCII(switches::kJsonWebSocketPort);
    unsigned int parsed_port;
    if (base::StringToUint(port_str, &parsed_port) && parsed_port <= 65535) {
      actual_port = static_cast<uint32_t>(parsed_port);
      LOG(INFO) << "[JsonWebSocketV2] Using command line port: " << actual_port;
    }
  }

  LOG(INFO) << "[JsonWebSocketV2] Connect request: " << host << ":" << actual_port << path;

  // Check if already connected
  if (websocket_manager_->IsConnected()) {
    LOG(INFO) << "[JsonWebSocketV2] Already connected";
    std::move(callback).Run(true);
    return;
  }

  // Connect using network WebSocket manager
  websocket_manager_->Connect(
      host, actual_port, path,
      base::BindOnce(&JsonWebSocketServiceImplV2::OnConnectionResult,
                    weak_factory_.GetWeakPtr(), std::move(callback)));
}

void JsonWebSocketServiceImplV2::SendJsonMessage(
    const std::string& json_message,
    SendJsonMessageCallback callback) {
  LOG(INFO) << "[JsonWebSocketV2] Send message request, size: " << json_message.size();

  if (!websocket_manager_->IsConnected()) {
    LOG(WARNING) << "[JsonWebSocketV2] Not connected";
    std::move(callback).Run(false);
    return;
  }

  // Send message through network WebSocket manager
  websocket_manager_->SendMessage(
      json_message,
      base::BindOnce(&JsonWebSocketServiceImplV2::OnMessageSendResult,
                    weak_factory_.GetWeakPtr(), std::move(callback)));
}

void JsonWebSocketServiceImplV2::Disconnect() {
  LOG(INFO) << "[JsonWebSocketV2] Disconnect request";
  if (websocket_manager_) {
    websocket_manager_->Disconnect();
  }
}

void JsonWebSocketServiceImplV2::IsConnected(IsConnectedCallback callback) {
  bool connected = websocket_manager_ && websocket_manager_->IsConnected();
  LOG(INFO) << "[JsonWebSocketV2] IsConnected: " << connected;
  std::move(callback).Run(connected);
}

void JsonWebSocketServiceImplV2::OnConnectionResult(ConnectCallback callback,
                                                    bool success) {
  LOG(INFO) << "[JsonWebSocketV2] Connection result: " << success;
  std::move(callback).Run(success);
}

void JsonWebSocketServiceImplV2::OnMessageSendResult(
    SendJsonMessageCallback callback,
    bool success) {
  LOG(INFO) << "[JsonWebSocketV2] Message send result: " << success;
  std::move(callback).Run(success);
}

}  // namespace content