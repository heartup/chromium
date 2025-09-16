#!/usr/bin/env python3
"""
简单的WebSocket测试服务器
用于接收来自V8 JsonStringify的消息
支持密钥加密验证功能
"""

import asyncio
import websockets
import json
import sys
from datetime import datetime

# 全局客户端集合
clients = set()

# 密钥验证配置
ENABLE_KEY_VERIFICATION = True  # 是否启用密钥验证
TEST_AUTH_KEY = "878fddae9fe548cdb5b2939aa38d6cf3"  # 测试用验证密钥，应与命令行参数--json-websocket-key一致

# 已验证的客户端集合
verified_clients = set()

def encrypt_key(key):
    """
    加密密钥 - 与C++端相同的算法
    1. 每个字符与其位置异或
    2. 然后循环移位
    3. 最后转换为十六进制字符串
    """
    encrypted = []

    for i, ch in enumerate(key):
        # 转换为字节值
        byte_val = ord(ch) if isinstance(ch, str) else ch

        # 与位置异或
        byte_val ^= (i & 0xFF)

        # 循环左移3位
        byte_val = ((byte_val << 3) | (byte_val >> 5)) & 0xFF

        # 与固定值异或增加复杂度
        byte_val ^= 0xA5

        # 转换为十六进制
        encrypted.append(f'{byte_val:02x}')

    result = ''.join(encrypted)
    print(f"Original key: {key}")
    print(f"Encrypted key: {result}")
    return result

async def register_client(websocket):
    """注册新的客户端连接"""
    clients.add(websocket)
    print(f"客户端已连接: {websocket.remote_address}")
    print(f"当前连接数: {len(clients)}")

async def unregister_client(websocket):
    """注销客户端连接"""
    clients.discard(websocket)
    verified_clients.discard(websocket)  # 同时从已验证集合中移除
    print(f"客户端已断开: {websocket.remote_address}")
    print(f"当前连接数: {len(clients)}")

async def verify_auth_key(websocket):
    """执行密钥验证 - 服务器主动发送加密密钥"""
    if not ENABLE_KEY_VERIFICATION:
        verified_clients.add(websocket)
        return True

    try:
        print(f"\n=== 密钥加密验证 ===")
        print(f"服务器密钥: {TEST_AUTH_KEY}")

        # 加密服务器端的密钥
        encrypted_server_key = encrypt_key(TEST_AUTH_KEY)
        print(f"服务器加密后密钥: {encrypted_server_key}")

        # 主动发送加密后的密钥给客户端
        print(f"发送加密密钥给客户端验证...")
        await websocket.send(encrypted_server_key)

        # 等待客户端验证响应（超时10秒）
        try:
            client_response = await asyncio.wait_for(websocket.recv(), timeout=10.0)
            print(f"收到客户端验证响应: {client_response}")

            # 检查客户端响应
            if client_response == "AUTH_SUCCESS":
                print("密钥验证成功! 客户端确认密钥匹配")
                verified_clients.add(websocket)
                return True
            elif client_response == "AUTH_FAILED":
                print("密钥验证失败! 客户端报告密钥不匹配")
                return False
            else:
                print(f"收到意外的客户端响应: {client_response}")
                return False

        except asyncio.TimeoutError:
            print("等待客户端验证响应超时")
            return False

    except Exception as e:
        print(f"密钥验证过程出错: {e}")
        return False

async def handle_message(websocket, message):
    """处理接收到的消息"""
    # 检查是否已通过密钥验证
    if ENABLE_KEY_VERIFICATION and websocket not in verified_clients:
        print(f"警告: 客户端未通过密钥验证，忽略消息")
        return

    print(f"\n=== 收到消息 ===")
    print(f"时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
    print(f"来自: {websocket.remote_address}")
    print(f"消息长度: {len(message)} 字节")

    # 只打印前500个字符，避免输出过长
    if len(message) > 500:
        print(f"消息内容(前500字符): {message[:500]}...")
    else:
        print(f"消息内容: {message}")
    print("=" * 40)

    # 立即刷新输出缓冲区，确保日志立即显示
    sys.stdout.flush()

    # 不发送响应，避免客户端混淆
    # 客户端的WebSocket库会将响应当作新消息处理

async def client_handler(websocket):
    """处理单个客户端连接"""
    print(f"\n新的连接请求 - {datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
    sys.stdout.flush()
    await register_client(websocket)

    # 设置ping间隔和超时
    websocket.ping_interval = None  # 禁用自动ping，让客户端控制
    websocket.ping_timeout = None   # 禁用ping超时
    websocket.close_timeout = 60    # 关闭连接的超时时间

    # 执行密钥验证
    if ENABLE_KEY_VERIFICATION:
        if not await verify_auth_key(websocket):
            print("密钥验证失败，关闭连接")
            await websocket.close()
            await unregister_client(websocket)
            return
        print("密钥验证通过，可以接收消息")

    try:
        async for message in websocket:
            await handle_message(websocket, message)
    except websockets.exceptions.ConnectionClosed as e:
        print(f"客户端连接已关闭 - {datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
        print(f"关闭原因: {e}")
        sys.stdout.flush()
    except websockets.exceptions.WebSocketException as e:
        print(f"WebSocket异常: {e}")
        sys.stdout.flush()
    except Exception as e:
        print(f"客户端处理错误: {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()
        sys.stdout.flush()
    finally:
        await unregister_client(websocket)

async def start_server():
    """启动WebSocket服务器"""
    host = 'localhost'
    port = 7746

    print(f"启动WebSocket服务器: ws://{host}:{port}")
    if ENABLE_KEY_VERIFICATION:
        print(f"密钥验证: 已启用")
        print(f"验证密钥: {TEST_AUTH_KEY}")
        print("注意: 客户端需使用 --json-websocket-key={} 参数启动".format(TEST_AUTH_KEY))
    else:
        print(f"密钥验证: 已禁用")
    print("等待来自V8 JsonStringify的连接...")
    print("按Ctrl+C停止服务器")
    print(f"启动时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    sys.stdout.flush()

    # 启动服务器并等待，禁用自动ping让客户端控制
    async with websockets.serve(
        client_handler,
        host,
        port,
        ping_interval=None,  # 禁用自动ping
        ping_timeout=None,   # 禁用ping超时
        close_timeout=60,    # 关闭超时
        max_size=10 * 1024 * 1024  # 最大消息10MB
    ) as server:
        print(f"服务器已启动在 {host}:{port}")
        sys.stdout.flush()
        await server.serve_forever()

def main():
    """主函数"""
    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\n服务器已停止")
    except Exception as e:
        print(f"服务器启动失败: {e}")

if __name__ == "__main__":
    main()
