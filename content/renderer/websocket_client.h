#ifndef V8_JSON_WEBSOCKET_CLIENT_H_
#define V8_JSON_WEBSOCKET_CLIENT_H_

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>

#if _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>

typedef SOCKET SocketHandle;
typedef SSIZE_T ssize_t;
#define CloseSocket closesocket
#define InvalidSocket INVALID_SOCKET
#define SocketGetLastError() WSAGetLastError()
static const int kErrInterrupt = WSAEINTR;

#else  // _WIN32

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

typedef int SocketHandle;
#define CloseSocket close
#define InvalidSocket (-1)
#define SocketGetLastError() errno
#define SOCKET_ERROR (-1)
static const int kErrInterrupt = EINTR;

#endif  // _WIN32

namespace blink::internal {

class WebSocketClient {
 public:
  WebSocketClient();
  ~WebSocketClient();

  // 连接到WebSocket服务器
  bool Connect(const std::string& host, int port, const std::string& path = "/");

  // 发送消息（带确认机制）
  bool SendMessage(const std::string& message);

  // 等待服务器响应
  bool WaitForResponse(int timeout_ms = 5000);

  // 断开连接
  void Disconnect();

  // 检查连接状态
  bool IsConnected() const;

  // 发送Ping帧
  bool SendPing();

  // 接收并处理帧（用于处理ping/pong）
  bool ReceiveFrame();

 private:
  // WebSocket握手
  bool PerformHandshake(const std::string& host, const std::string& path);

  // 发送WebSocket帧
  bool SendFrame(const std::string& data);

  // 创建WebSocket key
  std::string CreateWebSocketKey();

  // Base64编码
  std::string Base64Encode(const std::string& data);

  // SHA1哈希
  std::string Sha1Hash(const std::string& data);

  SocketHandle socket_fd_;
  std::atomic<bool> connected_;
  std::string host_;
  int port_;
  std::mutex send_mutex_;
};

// WebSocket客户端由内部函数管理，不需要外部访问全局实例

// 初始化WebSocket客户端
bool InitializeWebSocketClient(const std::string& host, int port, const std::string& path = "/");

// 发送JSON消息到WebSocket服务器
bool SendJsonToWebSocket(const std::string& json_message);

// 清理WebSocket客户端
void CleanupWebSocketClient();

} // namespace blink::internal


#endif  // V8_JSON_WEBSOCKET_CLIENT_H_
