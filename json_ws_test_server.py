#!/usr/bin/env python3
"""
简单的WebSocket测试服务器
用于接收来自V8 JsonStringify的消息
支持MAC地址验证功能
"""

import asyncio
import websockets
import json
import sys
from datetime import datetime

# 全局客户端集合
clients = set()

# MAC地址验证配置
ENABLE_MAC_VERIFICATION = True  # 是否启用MAC地址验证
TEST_MAC_ADDRESS = "20:0d:b0:1c:1b:05"  # 测试用MAC地址，请修改为你的实际MAC地址

# 已验证的客户端集合
verified_clients = set()

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

async def verify_mac_address(websocket):
    """执行MAC地址验证"""
    if not ENABLE_MAC_VERIFICATION:
        verified_clients.add(websocket)
        return True

    try:
        print(f"\n=== MAC地址验证 ===")
        print(f"发送MAC地址给客户端验证: {TEST_MAC_ADDRESS}")

        # 发送MAC地址给客户端
        await websocket.send(TEST_MAC_ADDRESS)

        # 等待客户端响应（超时10秒）
        try:
            response = await asyncio.wait_for(websocket.recv(), timeout=10.0)
            print(f"收到验证响应: {response}")

            if response == "MAC_VERIFIED":
                print("MAC地址验证成功!")
                verified_clients.add(websocket)
                return True
            elif response == "MAC_VERIFICATION_FAILED":
                print("MAC地址验证失败!")
                return False
            else:
                print(f"意外的验证响应: {response}")
                return False

        except asyncio.TimeoutError:
            print("MAC地址验证超时")
            return False

    except Exception as e:
        print(f"MAC地址验证过程出错: {e}")
        return False

async def handle_message(websocket, message):
    """处理接收到的消息"""
    # 检查是否已通过MAC验证
    if ENABLE_MAC_VERIFICATION and websocket not in verified_clients:
        print(f"警告: 客户端未通过MAC验证，忽略消息")
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

    # 执行MAC地址验证
    if ENABLE_MAC_VERIFICATION:
        if not await verify_mac_address(websocket):
            print("MAC地址验证失败，关闭连接")
            await websocket.close()
            await unregister_client(websocket)
            return
        print("MAC地址验证通过，可以接收消息")

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
    port = 7779

    print(f"启动WebSocket服务器: ws://{host}:{port}")
    if ENABLE_MAC_VERIFICATION:
        print(f"MAC地址验证: 已启用")
        print(f"验证MAC地址: {TEST_MAC_ADDRESS}")
        print("注意: 请确保TEST_MAC_ADDRESS与客户端MAC地址一致")
    else:
        print(f"MAC地址验证: 已禁用")
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
