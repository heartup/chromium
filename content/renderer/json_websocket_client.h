#ifndef CONTENT_RENDERER_JSON_WEBSOCKET_CLIENT_H_
#define CONTENT_RENDERER_JSON_WEBSOCKET_CLIENT_H_

#include <cstdint>
#include <string>

#include "base/memory/singleton.h"
#include "content/common/json_websocket.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {

class RenderFrameImpl;

// Client class for communicating with JsonWebSocketService in browser process
// This runs in the renderer process and uses Mojo IPC to bypass sandbox restrictions
class JsonWebSocketClient {
 public:
  // Get singleton instance
  static JsonWebSocketClient* GetInstance();

  // Initialize the Mojo connection
  void Initialize(RenderFrameImpl* render_frame);

  // Connect to WebSocket server (via browser process)
  bool Connect(const std::string& host, uint32_t port, const std::string& path);

  // Send JSON message (via browser process)
  bool SendJsonMessage(const std::string& json_message);

  // Disconnect from WebSocket server
  void Disconnect();

  // Check if connected
  bool IsConnected();

 private:
  friend struct base::DefaultSingletonTraits<JsonWebSocketClient>;

  JsonWebSocketClient();
  ~JsonWebSocketClient();

  // Ensure service is bound
  void EnsureServiceBound(RenderFrameImpl* render_frame);

  // Mojo remote to the browser process service
  mojo::Remote<mojom::JsonWebSocketService> service_;

  // Cached connection state
  bool connected_ = false;
};

// Helper functions for global access
bool InitializeJsonWebSocketClient(RenderFrameImpl* render_frame);
bool SendJsonViaIPC(const std::string& json_message);

}  // namespace content

#endif  // CONTENT_RENDERER_JSON_WEBSOCKET_CLIENT_H_