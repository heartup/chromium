#!/usr/bin/env python3
"""
测试密钥加密算法
验证Python和C++端的加密算法是否一致
"""

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
    return result

def test_encryption():
    """测试几个密钥的加密"""
    test_keys = [
        "878fddae9fe548cdb5b2939aa38d6cf3",
        "test123",
        "abcdef",
        "1234567890",
    ]

    print("密钥加密测试")
    print("=" * 50)

    for key in test_keys:
        encrypted = encrypt_key(key)
        print(f"原始密钥: {key}")
        print(f"加密结果: {encrypted}")
        print(f"长度: 原始={len(key)}, 加密={len(encrypted)}")
        print("-" * 50)

    # 测试默认密钥
    default_key = "878fddae9fe548cdb5b2939aa38d6cf3"
    encrypted = encrypt_key(default_key)

    print("\n默认密钥的加密结果（用于验证C++端）：")
    print(f"密钥: {default_key}")
    print(f"加密: {encrypted}")
    print("\n在C++端应该得到相同的加密结果")

if __name__ == "__main__":
    test_encryption()