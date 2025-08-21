#include "content/renderer/websocket_client.h"

#include <cstring>
#include <sstream>
#include <random>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <ranges>

#include "base/logging.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"

#if _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace blink::internal {

#if _WIN32
// Windows 网络初始化
static bool InitializeWinsock() {
  WSADATA wsa_data;
  int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  if (result != 0) {
    LOG(ERROR) << "WSAStartup failed with error: " << result;
    return false;
  }
  return true;
}

// Windows 网络清理
static void CleanupWinsock() {
  WSACleanup();
}
#endif

// 全局WebSocket客户端管理
static WebSocketClient*& GetGlobalWebSocketClientRef() {
  static WebSocketClient* g_websocket_client = nullptr;
  return g_websocket_client;
}

WebSocketClient::WebSocketClient()
    : socket_fd_(InvalidSocket), connected_(false), port_(0) {}

WebSocketClient::~WebSocketClient() {
  Disconnect();
}

bool WebSocketClient::Connect(const std::string& host, int port, const std::string& path) {
  if (connected_.load()) {
    return true;
  }

#if _WIN32
  // 初始化 Winsock (Windows)
  if (!InitializeWinsock()) {
    return false;
  }
#endif

  host_ = host;
  port_ = port;

  // 创建socket
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "Failed to create socket, error: " << SocketGetLastError();
#if _WIN32
    CleanupWinsock();
#endif
    return false;
  }

  // 解析主机地址
  struct hostent* server = gethostbyname(host.c_str());
  if (server == nullptr) {
    LOG(ERROR) << "Failed to resolve hostname: " << host << ", error: " << SocketGetLastError();
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
#if _WIN32
    CleanupWinsock();
#endif
    return false;
  }

  // 设置服务器地址
  struct sockaddr_in server_addr = {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(static_cast<uint16_t>(port));
  // SAFETY: server->h_addr points to server->h_length bytes of address data
  // Copy the address bytes using UNSAFE_BUFFERS for C API compatibility
  if (server->h_length == sizeof(server_addr.sin_addr.s_addr)) {
    UNSAFE_BUFFERS({
      std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    });
  }

  // 连接到服务器
  if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) != 0) {
    LOG(ERROR) << "Failed to connect to server, error: " << SocketGetLastError();
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
#if _WIN32
    CleanupWinsock();
#endif
    return false;
  }

  // 执行WebSocket握手
  if (!PerformHandshake(host, path)) {
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
#if _WIN32
    CleanupWinsock();
#endif
    return false;
  }

  connected_ = true;
  return true;
}

bool WebSocketClient::PerformHandshake(const std::string& host, const std::string& path) {
  std::string websocket_key = CreateWebSocketKey();

  std::ostringstream request;
  request << "GET " << path << " HTTP/1.1\r\n"
          << "Host: " << host << ":" << port_ << "\r\n"
          << "Upgrade: websocket\r\n"
          << "Connection: Upgrade\r\n"
          << "Sec-WebSocket-Key: " << websocket_key << "\r\n"
          << "Sec-WebSocket-Version: 13\r\n"
          << "User-Agent: V8-WebSocket-Client/1.0\r\n"
          << "Origin: http://" << host << ":" << port_ << "\r\n"
          << "Cache-Control: no-cache\r\n"
          << "Pragma: no-cache\r\n"
          << "\r\n";

  std::string request_str = request.str();

  // 添加调试输出
  LOG(INFO) << "WebSocket handshake request:\n" << request_str;

  ssize_t send_result = send(socket_fd_, request_str.c_str(), static_cast<int>(request_str.length()), 0);
  if (send_result == SOCKET_ERROR) {
    LOG(ERROR) << "Failed to send handshake request, error: " << SocketGetLastError();
    return false;
  }

  // 读取响应
  std::vector<char> buffer(1024);
  ssize_t bytes_received = recv(socket_fd_, buffer.data(), static_cast<int>(buffer.size() - 1), 0);
  if (bytes_received == SOCKET_ERROR || bytes_received == 0) {
    LOG(ERROR) << "Failed to receive handshake response, error: " << SocketGetLastError();
    return false;
  }

  // 安全地设置字符串结束符
  if (static_cast<size_t>(bytes_received) < buffer.size()) {
    buffer[static_cast<size_t>(bytes_received)] = '\0';
  }
  std::string response(buffer.data());

  // 添加调试输出
  LOG(INFO) << "WebSocket handshake response:\n" << response;

  // 检查是否包含升级确认
  bool has_101 = response.find("HTTP/1.1 101") != std::string::npos;
  bool has_upgrade = response.find("Upgrade: websocket") != std::string::npos ||
                     response.find("upgrade: websocket") != std::string::npos;

  LOG(INFO) << "Handshake check - 101: " << (has_101 ? "YES" : "NO") 
            << ", Upgrade: " << (has_upgrade ? "YES" : "NO");

  return has_101 && has_upgrade;
}

bool WebSocketClient::SendMessage(const std::string& message) {
  if (!connected_.load()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(send_mutex_);
  return SendFrame(message);
}

bool WebSocketClient::SendFrame(const std::string& data) {
  size_t data_length = data.length();
  std::vector<uint8_t> frame;

  // 构建WebSocket帧头
  uint8_t first_byte = 0x80 | 0x01;  // FIN + text frame
  frame.push_back(first_byte);

  // 设置payload长度
  if (data_length < 126) {
    frame.push_back(0x80 | static_cast<uint8_t>(data_length));  // MASK + length
  } else if (data_length < 65536) {
    frame.push_back(0x80 | 126);  // MASK + 126
    frame.push_back((data_length >> 8) & 0xFF);
    frame.push_back(data_length & 0xFF);
  } else {
    frame.push_back(0x80 | 127);  // MASK + 127
    for (int i = 7; i >= 0; i--) {
      frame.push_back((data_length >> (i * 8)) & 0xFF);
    }
  }

  // 生成掩码键
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
  uint32_t mask_key = dis(gen);

  // 添加掩码键
  frame.push_back((mask_key >> 24) & 0xFF);
  frame.push_back((mask_key >> 16) & 0xFF);
  frame.push_back((mask_key >> 8) & 0xFF);
  frame.push_back(mask_key & 0xFF);

  // 添加掩码后的payload
  for (size_t i = 0; i < data_length; i++) {
    uint8_t mask_byte = (mask_key >> ((3 - (i % 4)) * 8)) & 0xFF;
    frame.push_back(data[i] ^ mask_byte);
  }

  // 发送帧
  ssize_t result = send(socket_fd_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  if (result == SOCKET_ERROR) {
    LOG(ERROR) << "Failed to send WebSocket frame, error: " << SocketGetLastError();
    connected_ = false;
    return false;
  }

  return true;
}

void WebSocketClient::Disconnect() {
  if (socket_fd_ != InvalidSocket) {
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
  }
  connected_ = false;
#if _WIN32
  CleanupWinsock();
#endif
}

bool WebSocketClient::IsConnected() const {
  return connected_.load();
}

std::string WebSocketClient::CreateWebSocketKey() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  std::string key;
  for (int i = 0; i < 16; i++) {
    key += static_cast<char>(dis(gen));
  }

  return Base64Encode(key);
}

std::string WebSocketClient::Base64Encode(const std::string& data) {
  const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  int val = 0, valb = -6;
  for (unsigned char c : data) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      result.push_back(chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
  while (result.size() % 4) result.push_back('=');
  return result;
}

// 全局函数实现
bool InitializeWebSocketClient(const std::string& host, int port, const std::string& path) {
  WebSocketClient*& client = GetGlobalWebSocketClientRef();
  if (client == nullptr) {
    client = new WebSocketClient();
  }

  return client->Connect(host, port, path);
}

bool SendJsonToWebSocket(const std::string& json_message) {
  WebSocketClient* client = GetGlobalWebSocketClientRef();
  if (client == nullptr || !client->IsConnected()) {
    return false;
  }

  return client->SendMessage(json_message);
}

void CleanupWebSocketClient() {
  WebSocketClient*& client = GetGlobalWebSocketClientRef();
  if (client != nullptr) {
    client->Disconnect();
    delete client;
    client = nullptr;
  }
}

}  // namespace blink::internal
