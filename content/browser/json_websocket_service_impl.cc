#include "content/browser/json_websocket_service_impl.h"

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/browser/websocket_client.h"
#include "content/public/common/content_switches.h"

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
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver,
    int32_t window_id)
    : receiver_(this, std::move(receiver)), window_id_(window_id) {
  LOG(INFO) << "[Browser] JsonWebSocketServiceImpl created with window_id: " << window_id_;
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
  // Override port with command line parameter if specified
  uint32_t actual_port = port;
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kJsonWebSocketPort)) {
    std::string port_str = command_line->GetSwitchValueASCII(switches::kJsonWebSocketPort);
    unsigned int parsed_port;
    if (base::StringToUint(port_str, &parsed_port) && parsed_port <= 65535) {
      actual_port = static_cast<uint32_t>(parsed_port);
      LOG(INFO) << "[Browser] Using WebSocket port from command line: " << actual_port;
    } else {
      LOG(WARNING) << "[Browser] Invalid port specified: " << port_str << ", using default: " << port;
    }
  }

  LOG(INFO) << "[Browser] IPC: Connect request received for " << host << ":" << port << path
            << " (using actual port: " << actual_port << ")";

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

  // Try to connect with actual port
  bool success = websocket_client_->Connect(host, actual_port, path);
  std::move(callback).Run(success);
}

void JsonWebSocketServiceImpl::SendJsonMessage(const std::string& json_message,
                                               SendJsonMessageCallback callback) {
  LOG(INFO) << "[Browser] IPC: SendJsonMessage request received, size: " << json_message.size()
            << ", window_id: " << window_id_;

  if (!websocket_client_ || !websocket_client_->IsConnected()) {
    LOG(WARNING) << "[Browser] WebSocket not connected";
    std::move(callback).Run(false);
    return;
  }

  // 使用带窗口ID的发送函数
  bool success = blink::internal::SendJsonToWebSocketWithWindowId(window_id_, json_message);
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
  new JsonWebSocketServiceImpl(std::move(receiver), -1);
}

// static
void JsonWebSocketServiceImpl::CreateWithWindowId(
    int32_t window_id,
    mojo::PendingReceiver<mojom::JsonWebSocketService> receiver) {
  // This creates a self-owned instance that will be deleted when the
  // mojo connection is closed
  new JsonWebSocketServiceImpl(std::move(receiver), window_id);
}

}  // namespace content