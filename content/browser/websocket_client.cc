#include "content/browser/websocket_client.h"

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
#include "base/command_line.h"
#include "content/public/common/content_switches.h"

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
    LOG(ERROR) << "[WebSocket] WSAStartup failed with error: " << result;
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
    : socket_fd_(InvalidSocket),
      connected_(false),
      mac_verified_(false),
      port_(0),
      heartbeat_running_(false),
      waiting_pong_(false) {}

WebSocketClient::~WebSocketClient() {
  StopHeartbeat();
  Disconnect();
}

bool WebSocketClient::Connect(const std::string& host, int port, const std::string& path) {
  std::lock_guard<std::mutex> lock(connect_mutex_);

  // 再次检查连接状态（双重检查锁定）
  if (connected_.load()) {
    LOG(INFO) << "[WebSocket] Already connected, skipping connection attempt";
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
  path_ = path;  // 保存路径用于重连

  // 创建socket
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "[WebSocket] Failed to create socket, error: " << SocketGetLastError();
    return false;
  }

  // 解析主机地址
  struct hostent* server = gethostbyname(host.c_str());
  if (server == nullptr) {
    LOG(ERROR) << "[WebSocket] Failed to resolve hostname: " << host << ", error: " << SocketGetLastError();
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
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
    LOG(ERROR) << "[WebSocket] Failed to connect to server, error: " << SocketGetLastError();
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
    return false;
  }

  // 执行WebSocket握手
  if (!PerformHandshake(host, path)) {
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
    return false;
  }

  connected_ = true;

  // 连接建立后，立即进行密钥验证
  if (!VerifyAuthKey()) {
    LOG(ERROR) << "[WebSocket] Auth key verification failed!";
    Disconnect();
    return false;
  }

  LOG(INFO) << "[WebSocket] Auth key verification successful";

  // 启动心跳线程
  StartHeartbeat();

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
  LOG(INFO) << "[WebSocket] WebSocket handshake request:\n" << request_str;

  ssize_t send_result = send(socket_fd_, request_str.c_str(), static_cast<int>(request_str.length()), 0);
  if (send_result == SOCKET_ERROR) {
    LOG(ERROR) << "[WebSocket] Failed to send handshake request, error: " << SocketGetLastError();
    return false;
  }

  // 读取响应
  std::vector<char> buffer(1024);
  ssize_t bytes_received = recv(socket_fd_, buffer.data(), static_cast<int>(buffer.size() - 1), 0);
  if (bytes_received == SOCKET_ERROR || bytes_received == 0) {
    LOG(ERROR) << "[WebSocket] Failed to receive handshake response, error: " << SocketGetLastError();
    return false;
  }

  // 安全地设置字符串结束符
  if (static_cast<size_t>(bytes_received) < buffer.size()) {
    buffer[static_cast<size_t>(bytes_received)] = '\0';
  }
  std::string response(buffer.data());

  // 添加调试输出
  LOG(INFO) << "[WebSocket] WebSocket handshake response:\n" << response;

  // 检查是否包含升级确认
  bool has_101 = response.find("HTTP/1.1 101") != std::string::npos;
  bool has_upgrade = response.find("Upgrade: websocket") != std::string::npos ||
                     response.find("upgrade: websocket") != std::string::npos;

  LOG(INFO) << "[WebSocket] Handshake check - 101: " << (has_101 ? "YES" : "NO")
            << ", Upgrade: " << (has_upgrade ? "YES" : "NO");

  return has_101 && has_upgrade;
}

bool WebSocketClient::SendMessage(const std::string& message) {
  if (!connected_.load()) {
    LOG(WARNING) << "[WebSocket] WebSocket not connected in SendMessage";
    return false;
  }

  // 检查MAC地址是否已验证
  if (!mac_verified_.load()) {
    LOG(ERROR) << "[WebSocket] Cannot send message: MAC address not verified";
    return false;
  }

  // 检查 socket 是否有效
  if (socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "[WebSocket] Invalid socket in SendMessage";
    connected_ = false;
    return false;
  }

  // 先处理任何待处理的ping帧
  ReceiveFrame();

  std::lock_guard<std::mutex> lock(send_mutex_);
  bool result = SendFrame(message);
  if (result) {
    LOG(INFO) << "[WebSocket] Message sent successfully via WebSocket";
    // 不等待响应，因为服务器的响应会被当作新消息处理
    // Python服务器会立即发送确认，但这会触发新的消息处理
    return true;
  } else {
    LOG(ERROR) << "[WebSocket] SendFrame failed in SendMessage";
    return false;
  }
}

bool WebSocketClient::WaitForResponse(int timeout_ms) {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "[WebSocket] WaitForResponse: Not connected or invalid socket";
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
    LOG(INFO) << "[WebSocket] WaitForResponse: Received frame with opcode: " << static_cast<int>(opcode);

    if (opcode == 0x01) {  // 文本帧
      // 读取完整消息
      recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      LOG(INFO) << "[WebSocket] Received text frame from server (response)";
      got_response = true;
    } else if (opcode == 0x09) {  // ping帧
      LOG(INFO) << "[WebSocket] Received ping frame while waiting for response";
      // 处理ping并继续等待
      ReceiveFrame();
      if (timeout_ms > 100) {
        return WaitForResponse(timeout_ms - 100);  // 递归调用，减少超时时间
      }
    } else if (opcode == 0x0A) {  // pong帧
      LOG(INFO) << "[WebSocket] Received pong frame while waiting for response";
      // 忽略pong帧，继续等待
      recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      if (timeout_ms > 100) {
        return WaitForResponse(timeout_ms - 100);
      }
    } else {
      LOG(WARNING) << "[WebSocket] Received unexpected frame type: " << static_cast<int>(opcode);
    }
  } else if (received == 0) {
    LOG(ERROR) << "[WebSocket] Connection closed by server";
    connected_ = false;
  } else {
    LOG(WARNING) << "[WebSocket] WaitForResponse: No data received within timeout";
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

bool WebSocketClient::SendCloseFrame(uint16_t close_code, const std::string& reason) {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    return false;
  }

  // 构建Close帧 (opcode = 0x08)
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | 0x08);  // FIN + close frame

  // 计算payload长度 (2字节close code + reason字符串)
  size_t payload_length = 2 + reason.length();

  if (payload_length < 126) {
    frame.push_back(0x80 | static_cast<uint8_t>(payload_length));  // MASK + length
  } else if (payload_length < 65536) {
    frame.push_back(0x80 | 126);  // MASK + 126
    frame.push_back((payload_length >> 8) & 0xFF);
    frame.push_back(payload_length & 0xFF);
  }

  // 添加4字节掩码键
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
  uint32_t mask_key = dis(gen);
  frame.push_back((mask_key >> 24) & 0xFF);
  frame.push_back((mask_key >> 16) & 0xFF);
  frame.push_back((mask_key >> 8) & 0xFF);
  frame.push_back(mask_key & 0xFF);

  // 添加掩码后的payload (close code + reason)
  // Close code (2 bytes, big-endian)
  uint8_t code_bytes[2] = {static_cast<uint8_t>((close_code >> 8) & 0xFF),
                           static_cast<uint8_t>(close_code & 0xFF)};
  for (int i = 0; i < 2; i++) {
    uint8_t mask_byte = (mask_key >> ((3 - (i % 4)) * 8)) & 0xFF;
    frame.push_back(code_bytes[i] ^ mask_byte);
  }

  // Reason string
  for (size_t i = 0; i < reason.length(); i++) {
    uint8_t mask_byte = (mask_key >> ((3 - ((i + 2) % 4)) * 8)) & 0xFF;
    frame.push_back(static_cast<uint8_t>(reason[i]) ^ mask_byte);
  }

  // 发送Close帧
  ssize_t result = send(socket_fd_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  if (result == SOCKET_ERROR) {
    int error_code = SocketGetLastError();
    LOG(ERROR) << "[WebSocket] Failed to send close frame, error: " << error_code;
    return false;
  }

  LOG(INFO) << "[WebSocket] Sent close frame with code " << close_code << " and reason: " << reason;
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

  // 如果是pong帧(0x0A)，更新最后接收时间
  if (opcode == 0x0A) {
    // 读取完整的pong帧
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
      // 跳过这么大的pong，不太可能
    }

    // 读取掩码（如果有）
    if (masked) {
      uint8_t mask[4];
      recv(socket_fd_, reinterpret_cast<char*>(mask), 4, 0);
    }

    // 读取并丢弃payload
    if (payload_length > 0) {
      std::vector<uint8_t> discard(payload_length);
      recv(socket_fd_, reinterpret_cast<char*>(discard.data()), payload_length, 0);
    }

    // 更新pong接收时间
    {
      std::lock_guard<std::mutex> lock(heartbeat_mutex_);
      last_pong_time_ = std::chrono::steady_clock::now();
      waiting_pong_ = false;
    }

    LOG(INFO) << "[WebSocket] Received pong response";
  }
  // 如果是ping帧(0x09)，发送pong响应
  else if (opcode == 0x09) {
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
    LOG(INFO) << "[WebSocket] Responded to ping with pong";
  }
  // 如果是close帧(0x08)，处理关闭请求
  else if (opcode == 0x08) {
    // 读取完整的close帧
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

    // 读取close frame的payload (close code + reason)
    uint16_t close_code = 1000;  // 默认正常关闭
    std::string close_reason;

    if (payload_length >= 2) {
      uint8_t code_bytes[2];
      recv(socket_fd_, reinterpret_cast<char*>(code_bytes), 2, 0);
      close_code = (code_bytes[0] << 8) | code_bytes[1];

      // 读取reason（如果有）
      if (payload_length > 2) {
        std::vector<uint8_t> reason_bytes(payload_length - 2);
        recv(socket_fd_, reinterpret_cast<char*>(reason_bytes.data()), payload_length - 2, 0);
        close_reason = std::string(reason_bytes.begin(), reason_bytes.end());
      }
    } else if (payload_length > 0) {
      // 如果有payload但少于2字节，仍需要读取并丢弃
      std::vector<uint8_t> discard(payload_length);
      recv(socket_fd_, reinterpret_cast<char*>(discard.data()), payload_length, 0);
    }

    LOG(INFO) << "[WebSocket] Received close frame with code " << close_code
              << " and reason: " << close_reason;

    // 发送close frame响应
    SendCloseFrame(close_code, "Client closing");

    // 标记为断开连接
    connected_ = false;

    // 恢复阻塞模式后关闭socket
    #ifdef _WIN32
      u_long mode2 = 0;
      ioctlsocket(socket_fd_, FIONBIO, &mode2);
    #else
      int flags2 = fcntl(socket_fd_, F_GETFL, 0);
      fcntl(socket_fd_, F_SETFL, flags2 & ~O_NONBLOCK);
    #endif

    return false;  // 返回false表示连接已关闭
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
  LOG(INFO) << "[WebSocket] SendFrame: Sending " << frame.size() << " bytes (payload: " << data_length << " bytes)";

  // 打印帧的前20个字节用于调试
  std::string frame_hex;
  for (size_t i = 0; i < std::min(size_t(20), frame.size()); i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", frame[i]);
    frame_hex += buf;
  }
  LOG(INFO) << "[WebSocket] Frame header (first 20 bytes): " << frame_hex;

  ssize_t result = send(socket_fd_, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
  if (result == SOCKET_ERROR) {
    int error_code = SocketGetLastError();
    LOG(ERROR) << "[WebSocket] Failed to send WebSocket frame, error code: " << error_code
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
      LOG(ERROR) << "[WebSocket] Connection lost, marking as disconnected";
      connected_ = false;
      // 关闭socket以便下次重新连接
      if (socket_fd_ != InvalidSocket) {
        CloseSocket(socket_fd_);
        socket_fd_ = InvalidSocket;
      }
    }
    return false;
  } else if (result != static_cast<ssize_t>(frame.size())) {
    LOG(ERROR) << "[WebSocket] SendFrame: Partial send! Sent " << result << " bytes out of " << frame.size();
    return false;
  }

  LOG(INFO) << "[WebSocket] SendFrame: Successfully sent " << result << " bytes";
  return true;
}

void WebSocketClient::Disconnect() {
  // 先停止心跳线程，避免它继续尝试重连
  StopHeartbeat();

  // 加锁保护断开操作
  std::lock_guard<std::mutex> lock(connect_mutex_);

  // 如果连接状态为true，发送close frame进行优雅关闭
  if (connected_.load() && socket_fd_ != InvalidSocket) {
    LOG(INFO) << "[WebSocket] Sending close frame before disconnect";
    SendCloseFrame(1000, "Normal closure");
    // 短暂等待以确保close frame发送完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (socket_fd_ != InvalidSocket) {
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
  }
  connected_ = false;
  mac_verified_ = false;  // 重置MAC验证状态

  // 注意：不在这里调用WSACleanup，因为可能有其他WebSocket实例或网络操作在使用
  // WSACleanup应该在程序结束时或确保没有其他网络操作时调用
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

std::string WebSocketClient::ReceiveMessage(int timeout_ms) {
  if (!connected_.load() || socket_fd_ == InvalidSocket) {
    LOG(ERROR) << "[WebSocket] ReceiveMessage: Not connected or invalid socket";
    return "";
  }

  // 设置socket超时
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

  // 读取帧头（2字节）
  uint8_t header[2];
  ssize_t received = recv(socket_fd_, reinterpret_cast<char*>(header), 2, 0);
  if (received != 2) {
    LOG(ERROR) << "[WebSocket] Failed to receive frame header";
    return "";
  }

  uint8_t opcode = header[0] & 0x0F;
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
    UNSAFE_BUFFERS({
      for (int i = 0; i < 8; i++) {
        payload_length = (payload_length << 8) | extended_length[i];
      }
    });
  }

  // 读取掩码（如果有）
  uint8_t mask[4] = {0};
  if (masked) {
    recv(socket_fd_, reinterpret_cast<char*>(mask), 4, 0);
  }

  // 读取payload
  std::vector<uint8_t> payload(payload_length);
  if (payload_length > 0) {
    ssize_t total_received = 0;
    while (total_received < static_cast<ssize_t>(payload_length)) {
      ssize_t bytes;
      UNSAFE_BUFFERS({
        bytes = recv(socket_fd_,
                     reinterpret_cast<char*>(payload.data() + total_received),
                     static_cast<int>(payload_length - total_received), 0);
      });
      if (bytes <= 0) {
        LOG(ERROR) << "[WebSocket] Failed to receive complete payload";
        return "";
      }
      total_received += bytes;
    }
  }

  // 解掩码（如果需要）
  if (masked) {
    UNSAFE_BUFFERS({
      for (size_t i = 0; i < payload_length; i++) {
        payload[i] ^= mask[i % 4];
      }
    });
  }

  // 恢复默认超时
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

  // 转换为字符串（如果是文本帧）
  if (opcode == 0x01) {  // 文本帧
    return std::string(payload.begin(), payload.end());
  }

  return "";
}

std::string WebSocketClient::GetAuthKey() {
  // 从命令行参数获取密钥
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kJsonWebSocketKey)) {
    std::string key = command_line->GetSwitchValueASCII(switches::kJsonWebSocketKey);
    LOG(INFO) << "[WebSocket] Using auth key from command line: " << key;
    return key;
  }

  // 使用默认密钥
  const char* default_key = "878fddae9fe548cdb5b2939aa38d6cf3";
  LOG(INFO) << "[WebSocket] Using default auth key: " << default_key;
  return default_key;
}

std::string WebSocketClient::EncryptKey(const std::string& key) {
  // 简单的加密算法：
  // 1. 每个字符与其位置异或
  // 2. 然后循环移位
  // 3. 最后转换为十六进制字符串

  std::string encrypted;
  encrypted.reserve(key.length() * 2);

  for (size_t i = 0; i < key.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(key[i]);

    // 与位置异或
    ch ^= (i & 0xFF);

    // 循环左移3位
    ch = ((ch << 3) | (ch >> 5)) & 0xFF;

    // 与固定值异或增加复杂度
    ch ^= 0xA5;

    // 转换为十六进制
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", ch);
    encrypted.append(hex);
  }

  LOG(INFO) << "[WebSocket] Original key: " << key;
  LOG(INFO) << "[WebSocket] Encrypted key: " << encrypted;

  return encrypted;
}

bool WebSocketClient::VerifyAuthKey() {
  // 获取本地密钥
  std::string local_key = GetAuthKey();
  if (local_key.empty()) {
    LOG(ERROR) << "[WebSocket] Failed to get auth key";
    return false;
  }

  // 加密本地密钥
  std::string encrypted_local_key = EncryptKey(local_key);

  // 从服务器接收加密后的密钥
  LOG(INFO) << "[WebSocket] Waiting for server's encrypted auth key...";
  std::string server_encrypted_key = ReceiveMessage(5000);
  if (server_encrypted_key.empty()) {
    LOG(ERROR) << "[WebSocket] Failed to receive encrypted key from server";
    SendFrame("AUTH_FAILED");
    return false;
  }

  LOG(INFO) << "[WebSocket] Server's encrypted key: " << server_encrypted_key;
  LOG(INFO) << "[WebSocket] Local encrypted key: " << encrypted_local_key;

  // 在客户端比较加密后的密钥
  if (server_encrypted_key == encrypted_local_key) {
    LOG(INFO) << "[WebSocket] Auth key verification successful - keys match!";
    mac_verified_ = true;

    // 发送验证成功响应给服务器
    SendFrame("AUTH_SUCCESS");
    return true;
  } else {
    LOG(ERROR) << "[WebSocket] Auth key verification failed - keys don't match!";
    LOG(ERROR) << "[WebSocket] Expected: " << encrypted_local_key;
    LOG(ERROR) << "[WebSocket] Received: " << server_encrypted_key;

    // 发送验证失败响应给服务器
    SendFrame("AUTH_FAILED");
    return false;
  }
}

// 全局函数实现
bool InitializeWebSocketClient(const std::string& host, int port, const std::string& path) {
  WebSocketClient*& client = GetGlobalWebSocketClientRef();

  // 如果客户端已存在且已连接，直接返回成功
  if (client != nullptr && client->IsConnected()) {
    LOG(INFO) << "[WebSocket] WebSocket client already connected, skipping initialization";
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
  LOG(INFO) << "[WebSocket] SendJsonToWebSocket called, message size: " << json_message.size();

  WebSocketClient* client = GetGlobalWebSocketClientRef();

  // 如果客户端不存在，创建新的
  if (client == nullptr) {
    LOG(INFO) << "[WebSocket] Creating new WebSocket client";
    client = new WebSocketClient();
    GetGlobalWebSocketClientRef() = client;

    // 首次连接使用 Connect
    if (!client->Connect("127.0.0.1", 7746, "/")) {
      LOG(ERROR) << "[WebSocket] Cannot establish initial connection to 127.0.0.1:7746";
      return false;
    }
    LOG(INFO) << "[WebSocket] Initial connection established successfully";
  } else {
    // 已有客户端，检查连接状态
    if (!client->IsConnected()) {
      LOG(INFO) << "[WebSocket] WebSocket not connected, attempting reconnect";
      // 使用 Reconnect 而不是 Connect，避免重复连接
      client->Reconnect();

      if (!client->IsConnected()) {
        LOG(ERROR) << "[WebSocket] Reconnect failed";
        return false;
      }
      LOG(INFO) << "[WebSocket] Reconnected successfully";
    }
  }

  // 发送消息
  LOG(INFO) << "[WebSocket] Attempting to send message...";
  bool result = client->SendMessage(json_message);
  if (!result) {
    LOG(ERROR) << "[WebSocket] Failed sending message content (first 200 chars): " << json_message.substr(0, 200);
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
#if _WIN32
  // 在全局清理时才调用WSACleanup
  CleanupWinsock();
#endif
}

// 心跳机制实现
void WebSocketClient::StartHeartbeat() {
  if (heartbeat_running_.load()) {
    return;
  }

  heartbeat_running_ = true;
  heartbeat_thread_ = std::thread(&WebSocketClient::HeartbeatThread, this);
  LOG(INFO) << "[WebSocket] Heartbeat thread started";
}

void WebSocketClient::StopHeartbeat() {
  if (!heartbeat_running_.load()) {
    return;
  }

  heartbeat_running_ = false;
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
  LOG(INFO) << "[WebSocket] Heartbeat thread stopped";
}

void WebSocketClient::HeartbeatThread() {
  LOG(INFO) << "[WebSocket] Heartbeat thread running";

  auto last_heartbeat = std::chrono::steady_clock::now();

  while (heartbeat_running_.load()) {
    // 检查连接状态
    if (!connected_.load()) {
      LOG(WARNING) << "[WebSocket] Connection lost, attempting to reconnect...";
      Reconnect();
      if (!connected_.load()) {
        // 重连失败，等待一段时间后再试
        std::this_thread::sleep_for(std::chrono::seconds(5));
        continue;
      }
      // 重连成功，重置心跳计时器
      last_heartbeat = std::chrono::steady_clock::now();
    }

    // 持续接收帧，处理服务器可能发送的Ping
    if (!ReceiveFrame()) {
      // ReceiveFrame 返回 false 表示连接可能有问题
      if (connected_.load()) {
        LOG(WARNING) << "[WebSocket] ReceiveFrame failed but still marked as connected";
        // 可能是连接断开了，下一轮循环会处理
      }
    }

    // 检查是否到了发送心跳的时间
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();

    if (elapsed < kHeartbeatIntervalSeconds) {
      // 还没到心跳时间，短暂休眠后继续监听
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    // 重置心跳计时器
    last_heartbeat = now;

    if (!heartbeat_running_.load()) {
      break;
    }

    // 发送Ping
    LOG(INFO) << "[WebSocket] Sending ping...";
    {
      std::lock_guard<std::mutex> lock(heartbeat_mutex_);
      if (SendPing()) {
        last_ping_time_ = std::chrono::steady_clock::now();
        waiting_pong_ = true;
      } else {
        LOG(ERROR) << "[WebSocket] Failed to send ping";
        Reconnect();
        continue;
      }
    }

    // 等待Pong响应，期间持续接收帧
    auto start_wait = std::chrono::steady_clock::now();
    bool pong_received = false;

    while (heartbeat_running_.load() && connected_.load()) {
      // 尝试接收帧（可能是Pong或服务器的Ping）
      ReceiveFrame();

      // 检查是否收到Pong
      {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        if (!waiting_pong_) {
          pong_received = true;
          break;
        }
      }

      // 检查是否超时
      auto current_time = std::chrono::steady_clock::now();
      auto wait_elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_wait).count();
      if (wait_elapsed >= kPongTimeoutSeconds) {
        break;
      }

      // 短暂休眠避免CPU占用过高
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 如果没有收到Pong，尝试重连
    if (!pong_received && heartbeat_running_.load()) {
      LOG(ERROR) << "[WebSocket] Pong timeout, reconnecting...";
      Reconnect();
    }
  }

  LOG(INFO) << "[WebSocket] Heartbeat thread exiting";
}

bool WebSocketClient::CheckPongTimeout() {
  std::lock_guard<std::mutex> lock(heartbeat_mutex_);

  if (!waiting_pong_) {
    // 已经收到pong
    return true;
  }

  auto now = std::chrono::steady_clock::now();
  auto time_since_ping = std::chrono::duration_cast<std::chrono::seconds>(
      now - last_ping_time_).count();

  if (time_since_ping >= kPongTimeoutSeconds) {
    LOG(WARNING) << "[WebSocket] Pong timeout detected, "
                 << time_since_ping << " seconds since last ping";
    return false;
  }

  return true;
}

void WebSocketClient::Reconnect() {
  std::lock_guard<std::mutex> lock(connect_mutex_);

  // 再次检查连接状态，避免重复重连
  if (connected_.load()) {
    LOG(INFO) << "[WebSocket] Already connected during reconnect attempt";
    return;
  }

  LOG(INFO) << "[WebSocket] Attempting to reconnect...";

  // 先确保旧连接完全关闭
  if (socket_fd_ != InvalidSocket) {
    CloseSocket(socket_fd_);
    socket_fd_ = InvalidSocket;
  }

  // 等待一小段时间，让旧连接完全关闭
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 再次检查连接状态（可能其他线程已经重连成功）
  if (connected_.load()) {
    LOG(INFO) << "[WebSocket] Connection established by another thread during wait";
    return;
  }

  // 尝试重新连接
  if (!host_.empty() && port_ > 0) {
    // 重新建立连接（不使用Connect因为会递归调用StartHeartbeat）
    #if _WIN32
    if (!InitializeWinsock()) {
      LOG(ERROR) << "[WebSocket] Failed to initialize Winsock for reconnect";
      return;
    }
    #endif

    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ == InvalidSocket) {
      LOG(ERROR) << "[WebSocket] Failed to create socket for reconnect";
      return;
    }

    struct hostent* server = gethostbyname(host_.c_str());
    if (server == nullptr) {
      LOG(ERROR) << "[WebSocket] Failed to resolve hostname for reconnect";
      CloseSocket(socket_fd_);
      socket_fd_ = InvalidSocket;
      return;
    }

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (server->h_length == sizeof(server_addr.sin_addr.s_addr)) {
      UNSAFE_BUFFERS({
        std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
      });
    }

    if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
                sizeof(server_addr)) != 0) {
      LOG(ERROR) << "[WebSocket] Failed to connect to server for reconnect";
      CloseSocket(socket_fd_);
      socket_fd_ = InvalidSocket;
      return;
    }

    if (!PerformHandshake(host_, path_)) {
      LOG(ERROR) << "[WebSocket] Handshake failed during reconnect";
      CloseSocket(socket_fd_);
      socket_fd_ = InvalidSocket;
      return;
    }

    // 设置连接状态前先检查是否被其他线程关闭
    if (socket_fd_ == InvalidSocket) {
      LOG(WARNING) << "[WebSocket] Socket closed during reconnect";
      return;
    }

    connected_ = true;

    // 重新进行密钥验证
    if (!VerifyAuthKey()) {
      LOG(ERROR) << "[WebSocket] Auth key verification failed during reconnect";
      connected_ = false;
      CloseSocket(socket_fd_);
      socket_fd_ = InvalidSocket;
      return;
    }

    LOG(INFO) << "[WebSocket] Reconnected successfully";

    // 重置心跳状态
    {
      std::lock_guard<std::mutex> heartbeat_lock(heartbeat_mutex_);
      waiting_pong_ = false;
    }
  }
}

}  // namespace blink::internal
