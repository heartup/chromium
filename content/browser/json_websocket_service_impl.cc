#include "content/browser/json_websocket_service_impl.h"

#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "content/browser/websocket_client.h"

namespace content {

namespace internal {
// Wrapper class for the existing WebSocket client
class WebSocketClient {
 public:
  WebSocketClient() = default;
  ~WebSocketClient() {
    Disconnect();
  }

  bool Connect(const std::string& host, uint32_t port, const std::string& path) {
    LOG(INFO) << "[Browser] Connecting to WebSocket: " << host << ":" << port << path;
    connected_ = blink::internal::InitializeWebSocketClient(host, port, path);
    if (connected_) {
      LOG(INFO) << "[Browser] WebSocket connected successfully";
    } else {
      LOG(ERROR) << "[Browser] WebSocket connection failed";
    }
    return connected_;
  }

  bool SendMessage(const std::string& message) {
    if (!connected_) {
      LOG(WARNING) << "[Browser] WebSocket not connected, cannot send message";
      return false;
    }
    LOG(INFO) << "[Browser] Sending WebSocket message, size: " << message.size();
    bool result = blink::internal::SendJsonToWebSocket(message);
    if (!result) {
      LOG(ERROR) << "[Browser] Failed to send WebSocket message";
      connected_ = false;  // Mark as disconnected on failure
    }
    return result;
  }

  void Disconnect() {
    if (connected_) {
      LOG(INFO) << "[Browser] Disconnecting WebSocket";
      blink::internal::CleanupWebSocketClient();
      connected_ = false;
    }
  }

  bool IsConnected() const {
    return connected_;
  }

 private:
  bool connected_ = false;
};
}  // namespace internal

JsonWebSocketServiceImpl::JsonWebSocketServiceImpl(
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver)
    : receiver_(this, std::move(receiver)) {
  LOG(INFO) << "[Browser] JsonWebSocketServiceImpl created";
}

JsonWebSocketServiceImpl::~JsonWebSocketServiceImpl() {
  LOG(INFO) << "[Browser] JsonWebSocketServiceImpl destroyed";
  if (websocket_client_) {
    websocket_client_->Disconnect();
  }
}

void JsonWebSocketServiceImpl::Connect(const std::string& host,
                                       uint32_t port,
                                       const std::string& path,
                                       ConnectCallback callback) {
  LOG(INFO) << "[Browser] IPC: Connect request received for " << host << ":" << port << path;

  // Create WebSocket client if not exists
  if (!websocket_client_) {
    websocket_client_ = std::make_unique<internal::WebSocketClient>();
  }

  // If already connected, return success
  if (websocket_client_->IsConnected()) {
    LOG(INFO) << "[Browser] Already connected";
    std::move(callback).Run(true);
    return;
  }

  // Try to connect
  bool success = websocket_client_->Connect(host, port, path);
  std::move(callback).Run(success);
}

void JsonWebSocketServiceImpl::SendJsonMessage(const std::string& json_message,
                                               SendJsonMessageCallback callback) {
  LOG(INFO) << "[Browser] IPC: SendJsonMessage request received, size: " << json_message.size();

  if (!websocket_client_ || !websocket_client_->IsConnected()) {
    LOG(WARNING) << "[Browser] WebSocket not connected";
    std::move(callback).Run(false);
    return;
  }

  bool success = websocket_client_->SendMessage(json_message);
  std::move(callback).Run(success);
}

void JsonWebSocketServiceImpl::Disconnect() {
  LOG(INFO) << "[Browser] IPC: Disconnect request received";
  if (websocket_client_) {
    websocket_client_->Disconnect();
  }
}

void JsonWebSocketServiceImpl::IsConnected(IsConnectedCallback callback) {
  bool connected = websocket_client_ && websocket_client_->IsConnected();
  LOG(INFO) << "[Browser] IPC: IsConnected request, result: " << connected;
  std::move(callback).Run(connected);
}

// static
void JsonWebSocketServiceImpl::Create(
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver) {
  // This creates a self-owned instance that will be deleted when the
  // mojo connection is closed
  new JsonWebSocketServiceImpl(std::move(receiver));
}

}  // namespace content