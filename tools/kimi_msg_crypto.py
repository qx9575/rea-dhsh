"""s2c 解密（FUN_00819610 / 新构建 0x819750 的等价实现）。

帧格式（线上与内存一致）：
  [u16 长度(不含本字段，明文)][加密载荷]
  载荷内：+0x00 u16 命令（解密后 0x30xx），+0x0e u32 密钥位置种子

算法（载荷范围内，原地）：
  pos = (u32(p+0x0e) - 0x0d) mod 0x1000
  逐字节 i ∈ [0, len)，跳过 i ∈ [0x0e, 0x12)：
      s = pos & 7
      b = ror8(b, s)          # (b >> s) | (b << (8-s))
      b = KEY[pos] ^ b
      pos = (pos + 1) mod 0x1000
  然后前 min(len,16) 字节：b = ror8(b, 3)

c2s 方向为会话密钥加密（FUN_00934e30 + FUN_00819850 初始化），
离线不可解（需登录期握手）；明文用 kimi_msg_hook.py 在 0x7a1080 拿。
"""
from __future__ import annotations

import struct

KEY_TABLE_VA = 0x00A43EB8
KEY_TABLE_RAW = KEY_TABLE_VA - 0x400000   # .rdata identity 映射
KEY_TABLE_SIZE = 0x1000

_KEY = None


def load_key(exe_path: str = r"D:\software\dhsh\shdata\Game.exe") -> bytes:
    global _KEY
    if _KEY is None:
        with open(exe_path, "rb") as f:
            f.seek(KEY_TABLE_RAW)
            _KEY = f.read(KEY_TABLE_SIZE)
        if len(_KEY) != KEY_TABLE_SIZE:
            raise ValueError("密钥表读取不足")
    return _KEY


def decrypt_payload(p: bytes, key: bytes | None = None) -> bytes:
    """解密一帧的载荷（不含 2 字节长度头）。"""
    if key is None:
        key = load_key()
    m = bytearray(p)
    n = len(m)
    if n < 0x12:
        return bytes(m)
    u2 = (struct.unpack_from("<I", m, 0x0E)[0] - 0x0D) & 0xFFFFFFFF
    if u2 > 0xFFF:
        u2 = 0
    i = 0
    while i < n:
        if i == 0x0E:
            i = 0x13   # C 语义：跳到 0x12 后循环尾还会 +1 → 跳过 0x0e..0x12
            if i >= n:
                break
        s = u2 & 7
        b = m[i]
        if s:
            b = ((b << (8 - s)) | (b >> s)) & 0xFF
        m[i] = key[u2] ^ b
        u2 = (u2 + 1) & 0xFFF
        i += 1
    for i in range(min(n, 16)):
        m[i] = ((m[i] >> 3) | ((m[i] << 5) & 0xFF)) & 0xFF
    return bytes(m)


def decrypt_frame(frame: bytes, key: bytes | None = None) -> bytes:
    """完整帧（含 u16 长度头）→ 解密后的完整帧。"""
    return frame[:2] + decrypt_payload(frame[2:], key)


def iter_frames(stream: bytes):
    """从 TCP 重组流按 [u16 len] 切帧，yield (offset, frame)。"""
    pos = 0
    while pos + 4 <= len(stream):
        ln = struct.unpack_from("<H", stream, pos)[0]
        if ln < 2 or pos + 2 + ln > len(stream):
            break
        yield pos, stream[pos:pos + 2 + ln]
        pos += 2 + ln
