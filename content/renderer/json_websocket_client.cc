#include "content/renderer/json_websocket_client.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/run_loop.h"
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

  bool success = false;
  // Mojo 调用是异步的，使用 base::RunLoop 等待响应
  base::RunLoop run_loop;
  service_->Connect(
      host, port, path,
      base::BindOnce(
          [](bool* out_success, base::RunLoop* loop, bool result) {
            *out_success = result;
            loop->Quit();
          },
          &success, &run_loop));
  run_loop.Run();

  if (success) {
    connected_ = true;
    LOG(INFO) << "[Renderer] Connection successful";
  } else {
    LOG(ERROR) << "[Renderer] Connection failed";
  }

  return success;
}

bool JsonWebSocketClient::SendJsonMessage(const std::string& json_message) {
  if (!service_.is_bound()) {
    LOG(ERROR) << "[Renderer] Service not bound, cannot send message";
    return false;
  }

  if (!connected_) {
    LOG(WARNING) << "[Renderer] Not connected, attempting to connect first";
    // Try to connect with default settings
    if (!Connect("127.0.0.1", 8080, "/")) {
      LOG(ERROR) << "[Renderer] Failed to establish connection";
      return false;
    }
  }

  LOG(INFO) << "[Renderer] Sending JSON message via IPC, size: " << json_message.size();

  bool success = false;
  // Mojo 调用是异步的，使用 base::RunLoop 等待响应
  base::RunLoop run_loop;
  service_->SendJsonMessage(
      json_message,
      base::BindOnce(
          [](bool* out_success, base::RunLoop* loop, bool result) {
            *out_success = result;
            loop->Quit();
          },
          &success, &run_loop));
  run_loop.Run();

  if (!success) {
    LOG(ERROR) << "[Renderer] Failed to send message via IPC";
    connected_ = false;  // Mark as disconnected
  }

  return success;
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

  bool connected = false;
  // Mojo 调用是异步的，使用 base::RunLoop 等待响应
  base::RunLoop run_loop;
  service_->IsConnected(
      base::BindOnce(
          [](bool* out_connected, base::RunLoop* loop, bool result) {
            *out_connected = result;
            loop->Quit();
          },
          &connected, &run_loop));
  run_loop.Run();

  connected_ = connected;
  return connected;
}

// Helper functions
bool InitializeJsonWebSocketClient(RenderFrameImpl* render_frame) {
  JsonWebSocketClient::GetInstance()->Initialize(render_frame);
  return JsonWebSocketClient::GetInstance()->Connect("127.0.0.1", 8080, "/");
}

bool SendJsonViaIPC(const std::string& json_message) {
  return JsonWebSocketClient::GetInstance()->SendJsonMessage(json_message);
}

}  // namespace content