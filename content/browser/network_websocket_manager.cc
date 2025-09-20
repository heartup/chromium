// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/network_websocket_manager.h"

#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/unguessable_token.h"
#include "crypto/sha2.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/cookies/site_for_cookies.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/origin.h"

#if BUILDFLAG(IS_ANDROID)
#include "base/android/build_info.h"
#endif

namespace content {

namespace {

constexpr net::NetworkTrafficAnnotationTag kWebSocketTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("network_websocket_manager", R"(
        semantics {
          sender: "Network WebSocket Manager"
          description:
            "WebSocket connection managed by Chrome's network service for "
            "browser-initiated real-time communication."
          trigger:
            "Browser process initiates WebSocket connection for internal "
            "communication needs."
          data: "Application-specific messages and authentication data."
          destination: OTHER
          destination_other: "WebSocket server specified by the browser."
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "This connection is managed internally by the browser."
          policy_exception_justification:
            "Not user-controllable. Required for browser functionality."
        })");

}  // namespace

NetworkWebSocketManager::NetworkWebSocketManager(
    network::mojom::NetworkContext* network_context)
    : network_context_(network_context),
      readable_watcher_(FROM_HERE,
                       mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC,
                       base::SequencedTaskRunner::GetCurrentDefault()),
      writable_watcher_(FROM_HERE,
                       mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC,
                       base::SequencedTaskRunner::GetCurrentDefault()) {
  DCHECK(network_context_);

#if BUILDFLAG(IS_ANDROID)
  // Register application state listener for Android
  app_status_listener_ = base::android::ApplicationStatusListener::New(
      base::BindRepeating([](NetworkWebSocketManager* manager,
                            base::android::ApplicationState state) {
        bool is_foreground =
            (state == base::android::APPLICATION_STATE_HAS_RUNNING_ACTIVITIES ||
             state == base::android::APPLICATION_STATE_HAS_PAUSED_ACTIVITIES);
        manager->OnApplicationStateChange(is_foreground);
      }, base::Unretained(this)));
#endif
}

NetworkWebSocketManager::~NetworkWebSocketManager() {
  Disconnect();
}

void NetworkWebSocketManager::Connect(const std::string& host,
                                      uint32_t port,
                                      const std::string& path,
                                      ConnectionCallback callback) {
  if (connected_ || connecting_) {
    LOG(WARNING) << "[NetworkWebSocket] Already connected or connecting";
    if (callback)
      std::move(callback).Run(connected_);
    return;
  }

  LOG(INFO) << "[NetworkWebSocket] Connecting to " << host << ":" << port << path;

  connecting_ = true;
  connection_callback_ = std::move(callback);

  // Build WebSocket URL
  current_url_ = GURL("ws://" + host + ":" + base::NumberToString(port) + path);

  // Prepare handshake client
  mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> handshake_client;
  handshake_receiver_.reset();
  handshake_receiver_.Bind(handshake_client.InitWithNewPipeAndPassReceiver());

  // Create WebSocket through network context
  std::vector<network::mojom::HttpHeaderPtr> additional_headers;
  additional_headers.push_back(network::mojom::HttpHeader::New(
      "User-Agent", "Chrome-NetworkWebSocket/1.0"));

  // Request WebSocket creation
  network_context_->CreateWebSocket(
      current_url_,
      {}, // requested_protocols
      net::SiteForCookies::FromUrl(current_url_),
      net::StorageAccessApiStatus::kNone,
      net::IsolationInfo::CreateTransient(
          base::UnguessableToken::Create()),
      std::move(additional_headers),
      network::mojom::kBrowserProcessId,
      url::Origin::Create(current_url_),
      network::mojom::kWebSocketOptionNone,
      net::MutableNetworkTrafficAnnotationTag(kWebSocketTrafficAnnotation),
      std::move(handshake_client),
      mojo::NullRemote(), // url_loader_network_observer
      mojo::NullRemote(), // auth_handler
      mojo::NullRemote(), // header_client
      std::nullopt        // throttling_profile_id
  );
}

void NetworkWebSocketManager::SendMessage(const std::string& message,
                                          MessageCallback callback) {
  if (!connected_) {
    LOG(ERROR) << "[NetworkWebSocket] Not connected";
    if (callback)
      std::move(callback).Run(false);
    return;
  }

  message_callback_ = std::move(callback);
  WriteToDataPipe(message);
}

void NetworkWebSocketManager::Disconnect() {
  LOG(INFO) << "[NetworkWebSocket] Disconnecting";

  StopHeartbeat();

  if (websocket_) {
    websocket_->StartClosingHandshake(1000, "Normal closure");
    websocket_.reset();
  }

  handshake_receiver_.reset();
  client_receiver_.reset();
  readable_pipe_.reset();
  writable_pipe_.reset();
  readable_watcher_.Cancel();
  writable_watcher_.Cancel();

  connected_ = false;
  connecting_ = false;
  authenticated_ = false;
  waiting_for_auth_ = false;
  expected_auth_key_.clear();
  message_buffer_.clear();
}

void NetworkWebSocketManager::OnApplicationStateChange(bool is_foreground) {
  LOG(INFO) << "[NetworkWebSocket] App state changed: "
            << (is_foreground ? "foreground" : "background");

  bool was_background = !app_in_foreground_;
  app_in_foreground_ = is_foreground;

  if (is_foreground && was_background) {
    // Returning from background
    auto background_duration = base::TimeTicks::Now() - background_time_;

    if (background_duration > base::Minutes(2)) {
      LOG(INFO) << "[NetworkWebSocket] Long background period (>2 min), reconnecting";
      HandleReconnect();
    } else if (connected_ && authenticated_) {
      // Short background period, test connection with heartbeat
      LOG(INFO) << "[NetworkWebSocket] Testing connection after background";
      SendPing();
    }
  } else if (!is_foreground) {
    // Going to background
    background_time_ = base::TimeTicks::Now();
    LOG(INFO) << "[NetworkWebSocket] App going to background, stopping heartbeat";

    // Stop heartbeat in background to avoid unnecessary traffic
    StopHeartbeat();
  }

}

// WebSocketHandshakeClient implementation
void NetworkWebSocketManager::OnOpeningHandshakeStarted(
    network::mojom::WebSocketHandshakeRequestPtr request) {
  LOG(INFO) << "[NetworkWebSocket] Handshake started for " << request->url;
}

void NetworkWebSocketManager::OnFailure(const std::string& message,
                                        int32_t net_error,
                                        int32_t response_code) {
  LOG(ERROR) << "[NetworkWebSocket] Connection failed: " << message
             << " (net_error=" << net_error
             << ", response_code=" << response_code << ")";

  connecting_ = false;
  connected_ = false;

  if (connection_callback_) {
    std::move(connection_callback_).Run(false);
  }

  // Schedule reconnect if needed
  if (app_in_foreground_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NetworkWebSocketManager::HandleReconnect,
                      weak_factory_.GetWeakPtr()),
        base::Seconds(5));
  }
}

void NetworkWebSocketManager::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::WebSocket> socket,
    mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
    network::mojom::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  LOG(INFO) << "[NetworkWebSocket] Connection established";

  connecting_ = false;
  connected_ = true;

  // Bind WebSocket interfaces
  websocket_.Bind(std::move(socket));
  client_receiver_.reset();
  client_receiver_.Bind(std::move(client_receiver));

  // Setup data pipes
  readable_pipe_ = std::move(readable);
  writable_pipe_ = std::move(writable);

  // Setup watchers for data pipes
  readable_watcher_.Watch(
      readable_pipe_.get(),
      MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      base::BindRepeating(&NetworkWebSocketManager::OnDataPipeReadable,
                         weak_factory_.GetWeakPtr()));

  writable_watcher_.Watch(
      writable_pipe_.get(),
      MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      base::BindRepeating(&NetworkWebSocketManager::OnDataPipeWritable,
                         weak_factory_.GetWeakPtr()));

  LOG(INFO) << "[NetworkWebSocket] Data pipe watchers setup complete";

  // Start receiving WebSocket frames
  LOG(INFO) << "[NetworkWebSocket] Starting to receive WebSocket frames";
  websocket_->StartReceiving();

  // Start authentication process (async)
  if (!PerformAuthentication()) {
    LOG(ERROR) << "[NetworkWebSocket] Failed to start authentication";
    Disconnect();
    if (connection_callback_) {
      std::move(connection_callback_).Run(false);
    }
    return;
  }

  // Note: Heartbeat will start after successful authentication
  // Connection callback will be called after auth completes
}

// WebSocketClient implementation
void NetworkWebSocketManager::OnDataFrame(bool fin,
                                          network::mojom::WebSocketMessageType type,
                                          uint64_t data_length) {
  LOG(INFO) << "[NetworkWebSocket] *** OnDataFrame called: fin=" << fin
            << ", type=" << static_cast<int>(type)
            << ", length=" << data_length;

  // Always read if there's data available
  if (data_length > 0) {
    LOG(INFO) << "[NetworkWebSocket] Reading " << data_length << " bytes from data pipe";
    ReadFromDataPipe();
  }

  // Process complete message when fin=true
  if (fin) {
    LOG(INFO) << "[NetworkWebSocket] Message complete (fin=true), buffer size: " << message_buffer_.size();
    if (!message_buffer_.empty()) {
      ProcessReceivedMessage(message_buffer_);
      message_buffer_.clear();
    } else {
      // Even if buffer is empty, there might be data in the pipe
      LOG(INFO) << "[NetworkWebSocket] Buffer empty, trying to read from pipe";
      ReadFromDataPipe();
      if (!message_buffer_.empty()) {
        LOG(INFO) << "[NetworkWebSocket] Found data in pipe, processing";
        ProcessReceivedMessage(message_buffer_);
        message_buffer_.clear();
      }
    }
  }
}

void NetworkWebSocketManager::OnDropChannel(bool was_clean,
                                           uint16_t code,
                                           const std::string& reason) {
  LOG(INFO) << "[NetworkWebSocket] Channel dropped: clean=" << was_clean
            << ", code=" << code << ", reason=" << reason;

  connected_ = false;
  authenticated_ = false;
  StopHeartbeat();

  // Auto-reconnect for any disconnection when in foreground
  // This ensures we maintain connection as long as browser is active
  if (app_in_foreground_) {
    LOG(INFO) << "[NetworkWebSocket] Will reconnect in 3 seconds";
    HandleReconnect();
  }
}

void NetworkWebSocketManager::OnClosingHandshake() {
  LOG(INFO) << "[NetworkWebSocket] Closing handshake received";
  websocket_->StartClosingHandshake(1000, "");
}

// Heartbeat implementation
void NetworkWebSocketManager::StartHeartbeat() {
  // Chrome's WebSocket implementation handles keepalive automatically
  // No need for application-level heartbeat
  LOG(INFO) << "[NetworkWebSocket] WebSocket layer handles keepalive automatically";
}

void NetworkWebSocketManager::StopHeartbeat() {
  // No heartbeat to stop since WebSocket layer handles it
}

void NetworkWebSocketManager::SendPing() {
  // Chrome's WebSocket implementation handles Ping/Pong automatically
  // This method is kept for compatibility but does nothing
  LOG(INFO) << "[NetworkWebSocket] WebSocket layer handles Ping/Pong automatically";
}

void NetworkWebSocketManager::OnPingTimeout() {
  // Not used since WebSocket layer handles Ping/Pong
}

void NetworkWebSocketManager::HandleReconnect() {
  if (connecting_ || !current_url_.is_valid())
    return;

  LOG(INFO) << "[NetworkWebSocket] Scheduling reconnect";

  // Save the URL before any cleanup
  GURL saved_url = current_url_;

  // Schedule reconnect with delay to avoid rapid loops
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&NetworkWebSocketManager::Connect,
                    weak_factory_.GetWeakPtr(),
                    saved_url.host(),
                    saved_url.EffectiveIntPort(),
                    saved_url.path(),
                    ConnectionCallback()),
      base::Seconds(3));
}

// Authentication (preserving existing mechanism)
bool NetworkWebSocketManager::PerformAuthentication() {
  LOG(INFO) << "[NetworkWebSocket] Starting authentication process";

  // Get and encrypt our auth key for comparison
  std::string auth_key = GetAuthKey();
  expected_auth_key_ = EncryptKey(auth_key);

  LOG(INFO) << "[NetworkWebSocket] Waiting for server auth key...";

  // Don't set authenticated yet - wait for server's key
  authenticated_ = false;
  waiting_for_auth_ = true;

  // Return true to indicate auth process started (not completed)
  // Actual authentication happens in ProcessReceivedMessage
  return true;
}

std::string NetworkWebSocketManager::GetAuthKey() {
  // Get auth key from command line or use default
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch("json-websocket-key")) {
    std::string key = command_line->GetSwitchValueASCII("json-websocket-key");
    LOG(INFO) << "[NetworkWebSocket] Using auth key from command line: " << key;
    return key;
  }

  // Use default key (same as original implementation)
  const char* default_key = "878fddae9fe548cdb5b2939aa38d6cf3";
  LOG(INFO) << "[NetworkWebSocket] Using default auth key: " << default_key;
  return default_key;
}

std::string NetworkWebSocketManager::EncryptKey(const std::string& key) {
  // Preserving the exact encryption algorithm from original implementation
  std::string encrypted;
  encrypted.reserve(key.length() * 2);

  for (size_t i = 0; i < key.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(key[i]);

    // XOR with position
    ch ^= (i & 0xFF);

    // Rotate left by 3 bits
    ch = ((ch << 3) | (ch >> 5)) & 0xFF;

    // XOR with fixed value for complexity
    ch ^= 0xA5;

    // Convert to hex
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", ch);
    encrypted.append(hex);
  }

  LOG(INFO) << "[NetworkWebSocket] Original key: " << key;
  LOG(INFO) << "[NetworkWebSocket] Encrypted key: " << encrypted;

  return encrypted;
}

// Data pipe handling
void NetworkWebSocketManager::ReadFromDataPipe() {
  if (!readable_pipe_.is_valid()) {
    LOG(ERROR) << "[NetworkWebSocket] Readable pipe is not valid";
    return;
  }

  while (true) {
    base::span<const uint8_t> buffer;
    MojoResult result = readable_pipe_->BeginReadData(
        MOJO_READ_DATA_FLAG_NONE, buffer);

    if (result == MOJO_RESULT_SHOULD_WAIT) {
      LOG(INFO) << "[NetworkWebSocket] No more data to read (SHOULD_WAIT)";
      return;
    }

    if (result != MOJO_RESULT_OK) {
      LOG(ERROR) << "[NetworkWebSocket] Failed to read from pipe: " << result;
      return;
    }

    LOG(INFO) << "[NetworkWebSocket] Read " << buffer.size() << " bytes from data pipe";
    message_buffer_.append(reinterpret_cast<const char*>(buffer.data()),
                          buffer.size());
    readable_pipe_->EndReadData(buffer.size());
  }
}

void NetworkWebSocketManager::WriteToDataPipe(const std::string& data) {
  if (!writable_pipe_.is_valid())
    return;

  // Convert string to uint8_t span for Mojo
  base::span<const uint8_t> bytes = base::as_bytes(base::span(data));
  size_t bytes_written = 0;
  MojoResult result = writable_pipe_->WriteData(
      bytes, MOJO_WRITE_DATA_FLAG_NONE, bytes_written);

  if (result != MOJO_RESULT_OK) {
    LOG(ERROR) << "[NetworkWebSocket] Failed to write to pipe: " << result;
    if (message_callback_) {
      std::move(message_callback_).Run(false);
    }
    return;
  }

  // Send the frame metadata
  if (websocket_) {
    websocket_->SendMessage(network::mojom::WebSocketMessageType::TEXT,
                           bytes_written);
  }

  if (message_callback_) {
    std::move(message_callback_).Run(true);
  }
}

void NetworkWebSocketManager::OnDataPipeWritable(MojoResult result) {
  // Handle writable signal if needed for queued messages
}

void NetworkWebSocketManager::OnDataPipeReadable(MojoResult result) {
  LOG(INFO) << "[NetworkWebSocket] Data pipe readable signal: " << result;

  if (result != MOJO_RESULT_OK) {
    LOG(ERROR) << "[NetworkWebSocket] Readable pipe error: " << result;
    return;
  }

  ReadFromDataPipe();
}

void NetworkWebSocketManager::ProcessReceivedMessage(const std::string& message) {
  LOG(INFO) << "[NetworkWebSocket] Received message: " << message.substr(0, 100);

  // Handle authentication flow first
  if (waiting_for_auth_ && !authenticated_) {
    LOG(INFO) << "[NetworkWebSocket] Received auth key from server: " << message;

    // Compare server's key with our encrypted key
    std::string response;
    if (message == expected_auth_key_) {
      LOG(INFO) << "[NetworkWebSocket] Authentication keys match";
      response = "AUTH_SUCCESS";
      authenticated_ = true;
      waiting_for_auth_ = false;

      // No need for application-level heartbeat - WebSocket layer handles it

      // Notify connection callback of success
      if (connection_callback_) {
        std::move(connection_callback_).Run(true);
      }
    } else {
      LOG(ERROR) << "[NetworkWebSocket] Authentication failed - key mismatch";
      LOG(ERROR) << "[NetworkWebSocket] Expected: " << expected_auth_key_;
      LOG(ERROR) << "[NetworkWebSocket] Received: " << message;
      response = "AUTH_FAILED";
      authenticated_ = false;
      waiting_for_auth_ = false;

      // Disconnect on auth failure
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&NetworkWebSocketManager::Disconnect,
                        weak_factory_.GetWeakPtr()));

      // Notify connection callback of failure
      if (connection_callback_) {
        std::move(connection_callback_).Run(false);
      }
    }

    // Send authentication response
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&NetworkWebSocketManager::WriteToDataPipe,
                      weak_factory_.GetWeakPtr(), response));
    return;
  }

  // Process application messages after authentication
  if (authenticated_) {
    LOG(INFO) << "[NetworkWebSocket] Processing application message: " << message.substr(0, 100);
    // Add your message processing logic here
  }
}

}  // namespace content