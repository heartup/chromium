#include "content/renderer/websocket_client.h"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <random>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <ranges>
#ifndef _WIN32
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#endif

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
    LOG(WARNING) << "WebSocket not connected in SendMessage";
    return false;
  }

  // 检查 socket 是否有效
  if (socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "Invalid socket in SendMessage";
    connected_ = false;
    return false;
  }

  // 先处理任何待处理的ping帧
  ReceiveFrame();

  std::lock_guard<std::mutex> lock(send_mutex_);
  bool result = SendFrame(message);
  if (result) {
    LOG(INFO) << "Message sent successfully via WebSocket";
    // 不等待响应，因为服务器的响应会被当作新消息处理
    // Python服务器会立即发送确认，但这会触发新的消息处理
    return true;
  } else {
    LOG(ERROR) << "SendFrame failed in SendMessage";
    LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - Failed to send message to server!";
    return false;
  }
}

bool WebSocketClient::WaitForResponse(int timeout_ms) {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "WaitForResponse: Not connected or invalid socket";
    return false;
  }

  // 设置socket超时
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

  // 尝试接收响应
  uint8_t buffer[1024];
  ssize_t received = recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), MSG_PEEK);

  bool got_response = false;

  if (received > 0) {
    // 检查是否是文本帧（响应消息）
    uint8_t opcode = buffer[0] & 0x0F;
    LOG(INFO) << "WaitForResponse: Received frame with opcode: " << static_cast<int>(opcode);

    if (opcode == 0x01) {  // 文本帧
      // 读取完整消息
      recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      LOG(INFO) << "Received text frame from server (response)";
      got_response = true;
    } else if (opcode == 0x09) {  // ping帧
      LOG(INFO) << "Received ping frame while waiting for response";
      // 处理ping并继续等待
      ReceiveFrame();
      if (timeout_ms > 100) {
        return WaitForResponse(timeout_ms - 100);  // 递归调用，减少超时时间
      }
    } else if (opcode == 0x0A) {  // pong帧
      LOG(INFO) << "Received pong frame while waiting for response";
      // 忽略pong帧，继续等待
      recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      if (timeout_ms > 100) {
        return WaitForResponse(timeout_ms - 100);
      }
    } else {
      LOG(WARNING) << "Received unexpected frame type: " << static_cast<int>(opcode);
    }
  } else if (received == 0) {
    LOG(ERROR) << "Connection closed by server";
    connected_ = false;
  } else {
    LOG(WARNING) << "WaitForResponse: No data received within timeout";
  }

  // 恢复默认超时
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

  return got_response;
}

bool WebSocketClient::SendPing() {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    return false;
  }

  // 构建Ping帧 (opcode = 0x09)
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | 0x09);  // FIN + ping frame
  frame.push_back(0x80);  // MASK + 0 length

  // 添加4字节掩码键（即使payload为空也需要）
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
  uint32_t mask_key = dis(gen);
  frame.push_back((mask_key >> 24) & 0xFF);
  frame.push_back((mask_key >> 16) & 0xFF);
  frame.push_back((mask_key >> 8) & 0xFF);
  frame.push_back(mask_key & 0xFF);

  // 发送Ping帧
  ssize_t result = send(socket_fd_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  if (result == SOCKET_ERROR) {
    int error_code = SocketGetLastError();
    if (error_code == ECONNRESET || error_code == EPIPE ||
        error_code == ENOTCONN || error_code == ECONNABORTED) {
      connected_ = false;
      if (socket_fd_ != InvalidSocket) {
        CloseSocket(socket_fd_);
        socket_fd_ = InvalidSocket;
      }
    }
    return false;
  }
  return true;
}

bool WebSocketClient::ReceiveFrame() {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    return false;
  }

  // 设置非阻塞模式以避免阻塞
  #ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_fd_, FIONBIO, &mode);
  #else
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
  #endif

  uint8_t header[2];
  ssize_t received = recv(socket_fd_, reinterpret_cast<char*>(header), 2, MSG_PEEK);

  if (received < 2) {
    // 没有数据或连接断开
    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      connected_ = false;
      return false;
    }
    return true; // 没有数据但连接正常
  }

  uint8_t opcode = header[0] & 0x0F;

  // 如果是ping帧(0x09)，发送pong响应
  if (opcode == 0x09) {
    // 读取完整的ping帧
    recv(socket_fd_, reinterpret_cast<char*>(header), 2, 0);

    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_length = header[1] & 0x7F;

    // 读取扩展长度（如果需要）
    if (payload_length == 126) {
      uint8_t extended_length[2];
      recv(socket_fd_, reinterpret_cast<char*>(extended_length), 2, 0);
      UNSAFE_BUFFERS({
        payload_length = (extended_length[0] << 8) | extended_length[1];
      });
    } else if (payload_length == 127) {
      uint8_t extended_length[8];
      recv(socket_fd_, reinterpret_cast<char*>(extended_length), 8, 0);
      payload_length = 0;
      // 使用 UNSAFE_BUFFERS 宏来处理数组访问
      UNSAFE_BUFFERS({
        for (int i = 0; i < 8; i++) {
          payload_length = (payload_length << 8) | extended_length[i];
        }
      });
    }

    // 读取掩码（如果有）
    if (masked) {
      uint8_t mask[4];
      recv(socket_fd_, reinterpret_cast<char*>(mask), 4, 0);
    }

    // 读取payload
    std::vector<uint8_t> payload(payload_length);
    if (payload_length > 0) {
      recv(socket_fd_, reinterpret_cast<char*>(payload.data()), payload_length, 0);
    }

    // 发送pong响应 (opcode = 0x0A)
    std::vector<uint8_t> pong_frame;
    pong_frame.push_back(0x80 | 0x0A);  // FIN + pong frame

    // 添加payload长度
    if (payload_length < 126) {
      pong_frame.push_back(0x80 | static_cast<uint8_t>(payload_length));  // MASK + length
    } else if (payload_length < 65536) {
      pong_frame.push_back(0x80 | 126);  // MASK + 126
      pong_frame.push_back((payload_length >> 8) & 0xFF);
      pong_frame.push_back(payload_length & 0xFF);
    }

    // 添加掩码
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    uint32_t mask_key = dis(gen);
    pong_frame.push_back((mask_key >> 24) & 0xFF);
    pong_frame.push_back((mask_key >> 16) & 0xFF);
    pong_frame.push_back((mask_key >> 8) & 0xFF);
    pong_frame.push_back(mask_key & 0xFF);

    // 添加掩码后的payload
    UNSAFE_BUFFERS({
      for (size_t i = 0; i < payload_length; i++) {
        uint8_t mask_byte = (mask_key >> ((3 - (i % 4)) * 8)) & 0xFF;
        pong_frame.push_back(payload[i] ^ mask_byte);
      }
    });

    // 发送pong帧
    send(socket_fd_, reinterpret_cast<const char*>(pong_frame.data()), pong_frame.size(), 0);
    LOG(INFO) << "Responded to ping with pong";
  }

  // 恢复阻塞模式
  #ifdef _WIN32
    u_long mode2 = 0;
    ioctlsocket(socket_fd_, FIONBIO, &mode2);
  #else
    int flags2 = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags2 & ~O_NONBLOCK);
  #endif

  return true;
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
  LOG(INFO) << "SendFrame: Sending " << frame.size() << " bytes (payload: " << data_length << " bytes)";

  // 打印帧的前20个字节用于调试
  std::string frame_hex;
  for (size_t i = 0; i < std::min(size_t(20), frame.size()); i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", frame[i]);
    frame_hex += buf;
  }
  LOG(INFO) << "Frame header (first 20 bytes): " << frame_hex;

  ssize_t result = send(socket_fd_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  if (result == SOCKET_ERROR) {
    int error_code = SocketGetLastError();
    LOG(ERROR) << "Failed to send WebSocket frame, error code: " << error_code
               << ", frame size: " << frame.size()
               << ", socket fd: " << socket_fd_;

    // 检查具体的错误类型
#if _WIN32
    if (error_code == WSAECONNRESET || error_code == WSAECONNABORTED ||
        error_code == WSAENETDOWN || error_code == WSAENOTCONN) {
#else
    if (error_code == ECONNRESET || error_code == EPIPE ||
        error_code == ENOTCONN || error_code == ECONNABORTED) {
#endif
      LOG(ERROR) << "Connection lost, marking as disconnected";
      connected_ = false;
      // 关闭socket以便下次重新连接
      if (socket_fd_ != InvalidSocket) {
        CloseSocket(socket_fd_);
        socket_fd_ = InvalidSocket;
      }
    }
    return false;
  } else if (result != static_cast<ssize_t>(frame.size())) {
    LOG(ERROR) << "SendFrame: Partial send! Sent " << result << " bytes out of " << frame.size();
    return false;
  }

  LOG(INFO) << "SendFrame: Successfully sent " << result << " bytes";
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

  // 如果客户端已存在且已连接，直接返回成功
  if (client != nullptr && client->IsConnected()) {
    LOG(INFO) << "WebSocket client already connected, skipping initialization";
    return true;
  }

  // 如果客户端不存在，创建新的
  if (client == nullptr) {
    client = new WebSocketClient();
  }

  // 尝试连接（Connect内部会检查connected_状态）
  return client->Connect(host, port, path);
}

bool SendJsonToWebSocket(const std::string& json_message) {
  LOG(INFO) << "SendJsonToWebSocket called, message size: " << json_message.size();

  WebSocketClient* client = GetGlobalWebSocketClientRef();

  // 如果客户端不存在，创建新的
  if (client == nullptr) {
    LOG(INFO) << "Creating new WebSocket client";
    client = new WebSocketClient();
    GetGlobalWebSocketClientRef() = client;
  }

  // 先尝试处理任何待处理的帧来检测连接是否活跃
  if (client->IsConnected()) {
    if (!client->ReceiveFrame()) {
      LOG(WARNING) << "Connection may be broken";
      client->Disconnect();
    }
  }

  // 如果未连接，建立新连接
  if (!client->IsConnected()) {
    LOG(INFO) << "WebSocket not connected, connecting to 127.0.0.1:8080";
    if (!client->Connect("127.0.0.1", 8080, "/")) {
      LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - Failed to connect to WebSocket server!";
      LOG(ERROR) << "Cannot establish connection to 127.0.0.1:8080";
      return false;
    }
    LOG(INFO) << "Connected successfully";
  }

  // 发送消息
  LOG(INFO) << "Attempting to send message...";
  bool result = client->SendMessage(json_message);
  if (!result) {
    LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - First send attempt failed!";
    LOG(ERROR) << "Failed to send message on first attempt, forcing reconnect";

    // 强制重新连接
    client->Disconnect();
    if (client->Connect("127.0.0.1", 8080, "/")) {
      LOG(INFO) << "Reconnected successfully, retrying message send";
      result = client->SendMessage(json_message);
      if (result) {
        LOG(INFO) << "Message sent successfully after reconnection";
      } else {
        LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - Failed to send message even after reconnection!";
        LOG(ERROR) << "Message lost, server did not receive the data";
      }
    } else {
      LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - Failed to reconnect WebSocket for retry!";
      LOG(ERROR) << "Unable to reestablish connection with server";
    }
  } else {
    LOG(INFO) << "Message sent successfully to server";
  }

  if (!result) {
    LOG(ERROR) << "xxxxxxxxxxxxxxxxxxxxxxxxxxx - CRITICAL: Message was NOT delivered to server!";
    LOG(ERROR) << "Message content (first 200 chars): " << json_message.substr(0, 200);
  }

  return result;
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
