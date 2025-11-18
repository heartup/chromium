#ifndef CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_H_
#define CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "content/common/json_websocket.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace content {

// Forward declaration
namespace internal {
class WebSocketClient;
}

// Implementation of JsonWebSocketService that runs in the browser process
// This allows WebSocket connections without sandbox restrictions
class JsonWebSocketServiceImpl : public mojom::JsonWebSocketService {
 public:
  explicit JsonWebSocketServiceImpl(
      mojo::PendingReceiver<mojom::JsonWebSocketService> receiver,
      int64_t window_id = -1);
  ~JsonWebSocketServiceImpl() override;

  // mojom::JsonWebSocketService implementation
  void Connect(const std::string& host,
               uint32_t port,
               const std::string& path,
               ConnectCallback callback) override;
  void SendJsonMessage(const std::string& json_message,
                      SendJsonMessageCallback callback) override;
  void Disconnect() override;
  void IsConnected(IsConnectedCallback callback) override;

  // Create and bind a new service instance
  static void Create(
      mojo::PendingReceiver<mojom::JsonWebSocketService> receiver);

  // Create with window ID
  static void CreateWithWindowId(
      int64_t window_id,
      mojo::PendingReceiver<mojom::JsonWebSocketService> receiver);

 private:
  mojo::Receiver<mojom::JsonWebSocketService> receiver_;
  std::unique_ptr<internal::WebSocketClient> websocket_client_;
  int64_t window_id_;

  base::WeakPtrFactory<JsonWebSocketServiceImpl> weak_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_JSON_WEBSOCKET_SERVICE_IMPL_H_