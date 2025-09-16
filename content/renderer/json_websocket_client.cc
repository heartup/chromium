#include "content/renderer/json_websocket_client.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "content/public/renderer/render_thread.h"
#include "content/renderer/render_frame_impl.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace content {

// static
JsonWebSocketClient* JsonWebSocketClient::GetInstance() {
  return base::Singleton<JsonWebSocketClient>::get();
}

JsonWebSocketClient::JsonWebSocketClient() {
  LOG(INFO) << "[Renderer] JsonWebSocketClient created";
}

JsonWebSocketClient::~JsonWebSocketClient() {
  LOG(INFO) << "[Renderer] JsonWebSocketClient destroyed";
  Disconnect();
}

void JsonWebSocketClient::Initialize(RenderFrameImpl* render_frame) {
  LOG(INFO) << "[Renderer] Initializing JsonWebSocketClient";
  EnsureServiceBound(render_frame);
}

void JsonWebSocketClient::EnsureServiceBound(RenderFrameImpl* render_frame) {
  if (service_.is_bound()) {
    return;
  }

  LOG(INFO) << "[Renderer] Binding to JsonWebSocketService";

  // Get the interface from the browser process
  mojo::PendingRemote<mojom::JsonWebSocketService> service;
  render_frame->GetBrowserInterfaceBroker().GetInterface(
      service.InitWithNewPipeAndPassReceiver());

  service_.Bind(std::move(service));

  // Set disconnect handler
  service_.set_disconnect_handler(base::BindOnce(
      []() {
        LOG(WARNING) << "[Renderer] JsonWebSocketService disconnected";
      }));
}

bool JsonWebSocketClient::Connect(const std::string& host,
                                  uint32_t port,
                                  const std::string& path) {
  if (!service_.is_bound()) {
    LOG(ERROR) << "[Renderer] Service not bound, cannot connect";
    return false;
  }

  LOG(INFO) << "[Renderer] Requesting connection to " << host << ":" << port << path;

  // 为了避免阻塞主线程，我们暂时假设连接会成功
  // 实际的连接状态会在异步回调中更新
  service_->Connect(
      host, port, path,
      base::BindOnce(
          [](JsonWebSocketClient* client, bool result) {
            if (result) {
              client->connected_ = true;
              LOG(INFO) << "[Renderer] Connection successful (async)";
            } else {
              client->connected_ = false;
              LOG(ERROR) << "[Renderer] Connection failed (async)";
            }
          },
          base::Unretained(this)));

  // 暂时返回 true，实际状态会异步更新
  connected_ = true;
  LOG(INFO) << "[Renderer] Connection request sent (assuming success)";
  return true;
}

bool JsonWebSocketClient::SendJsonMessage(const std::string& json_message) {
  if (!service_.is_bound()) {
    LOG(ERROR) << "[Renderer] Service not bound, cannot send message";
    return false;
  }

  if (!connected_) {
    LOG(WARNING) << "[Renderer] Not connected, attempting to connect first";

    // Port configuration is handled in browser process
    uint32_t port = 7746;  // Default port (will be overridden in browser if configured)

    // Try to connect
    if (!Connect("127.0.0.1", port, "/")) {
      LOG(ERROR) << "[Renderer] Failed to establish connection";
      return false;
    }
  }

  LOG(INFO) << "[Renderer] Sending JSON message via IPC, size: " << json_message.size();

  // 异步发送，不阻塞主线程
  service_->SendJsonMessage(
      json_message,
      base::BindOnce(
          [](const std::string& msg, bool result) {
            if (result) {
              LOG(INFO) << "[Renderer] Message sent successfully (async)";
            } else {
              LOG(ERROR) << "[Renderer] Failed to send message (async)";
            }
          },
          json_message));

  // 假设发送成功（实际结果会在回调中报告）
  return true;
}

void JsonWebSocketClient::Disconnect() {
  if (service_.is_bound()) {
    LOG(INFO) << "[Renderer] Disconnecting";
    service_->Disconnect();
    connected_ = false;
  }
}

bool JsonWebSocketClient::IsConnected() {
  if (!service_.is_bound()) {
    return false;
  }

  // 返回缓存的连接状态，避免阻塞
  // 可以定期异步更新这个状态
  return connected_;
}

// Helper functions
bool InitializeJsonWebSocketClient(RenderFrameImpl* render_frame) {
  JsonWebSocketClient::GetInstance()->Initialize(render_frame);

  // Port configuration is handled in browser process via --json-websocket-port flag
  // Renderer always sends default port, browser will override if needed
  uint32_t port = 7746;  // Default port (will be overridden in browser if configured)

  LOG(INFO) << "[Renderer] Requesting connection (port will be configured in browser process)";
  return JsonWebSocketClient::GetInstance()->Connect("127.0.0.1", port, "/");
}

bool SendJsonViaIPC(const std::string& json_message) {
  return JsonWebSocketClient::GetInstance()->SendJsonMessage(json_message);
}

}  // namespace content