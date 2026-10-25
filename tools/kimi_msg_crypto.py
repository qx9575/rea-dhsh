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

c2s 方向（已破解，纯离线可解）：
  帧格式：[u16le 总长度 L(明文)][L 字节加密区][4 字节 MD5 尾（仅 RC4 帧）]
  - 首帧（登录 0x4004，长度 0x6f）：Blowfish-ECB 静态主密钥（FUN_00819850 路径2
    → FUN_00819760 逐 8 字节 FUN_00819160，末尾 CTS 交叠）。
    明文 = [04 40][16B 头][93B 凭据块]，凭据块：+0 账号、+0x33 密码、+0x47 u32 会话值。
  - 后续帧：RC4（FUN_00934e30 标准 PRGA，状态跨帧连续），
    KSA（FUN_009351f0 标准）密钥 = 93B 凭据块 + MD5大写hex(凭据块)[32B] = 125B。
    尾 = MD5大写hex(密文[:min(L-4,256)])[0:4] 的 4 个 ASCII 字符。
  密钥派生全部静态/登录帧自带 → 离线解密不需要任何内存读取。
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


# ==================== c2s: Blowfish + RC4 ====================

P_INIT_RAW = 0x643E70      # P 数组初值 18 dword（π 小数），磁盘文件偏移
MASTER_KEY_RAW = 0x644EB8  # 主密钥字符串（103B），磁盘文件偏移

_STATIC = None


def _load_static(exe_path: str = r"D:\software\dhsh\shdata\Game.exe"):
    """读取 P 初值(72B) + S 盒初值(0x1000B) + 主密钥字符串。"""
    global _STATIC
    if _STATIC is None:
        with open(exe_path, "rb") as f:
            f.seek(P_INIT_RAW)
            p_init = f.read(0x48)
            f.seek(KEY_TABLE_RAW)
            s_init = f.read(KEY_TABLE_SIZE)
            f.seek(MASTER_KEY_RAW)
            raw = f.read(256)
        if len(p_init) != 0x48 or len(s_init) != KEY_TABLE_SIZE:
            raise ValueError("静态表读取不足")
        master = raw[: raw.index(0)]
        _STATIC = (list(struct.unpack("<18I", p_init)),
                   list(struct.unpack("<1024I", s_init)), master)
    return _STATIC


class Blowfish:
    """FUN_00819160(加密)/FUN_00819330(解密)/FUN_00819510(密钥编排) 的逐操作移植。

    状态：S[1024] + P[18]（全部 dword）。
    F(x) = ((S1[x>>8&0xff] + S0[x&0xff]) ^ S2[x>>16&0xff]) + S3[x>>24]  (mod 2^32)
    """

    def __init__(self, key: bytes, exe_path: str = r"D:\software\dhsh\shdata\Game.exe"):
        p_init, s_init, _ = _load_static(exe_path)
        # 密钥编排 FUN_00819510：P[j] ^= (k[i3] | k[i2]<<8 | k[i4]<<16) << 8（循环）
        # 即 k[4j+3]<<8 | k[4j+2]<<16 | k[4j+1]<<24；每 4 字节组的首字节被跳过
        # 注意：C 源码按 *(char*) 读取（有符号扩展），密钥字节 ≥0x80 时高位被 0xff 污染
        P = list(p_init)
        idx = 0
        klen = len(key)
        sx = [b - 256 if b >= 128 else b for b in key]  # signed char 语义
        for j in range(18):
            i4 = idx + 1
            if i4 >= klen:
                i4 = 0
            i2 = i4 + 1
            if i2 >= klen:
                i2 = 0
            i3 = i2 + 1
            if i3 >= klen:
                i3 = 0
            idx = i3 + 1
            if idx >= klen:
                idx = 0
            P[j] ^= ((sx[i3] | (sx[i2] << 8) | (sx[i4] << 16)) << 8) & 0xFFFFFFFF
            P[j] &= 0xFFFFFFFF
        self.P = P
        self.S = list(s_init)
        # 链式填 P[0..17] 再填 S[0..1023]
        lo, hi = 0, 0
        for k in range(9):
            lo, hi = self._enc(lo, hi)
            self.P[2 * k] = lo
            self.P[2 * k + 1] = hi
        for k in range(0, 1024, 2):
            lo, hi = self._enc(lo, hi)
            self.S[k] = lo
            self.S[k + 1] = hi

    def _f(self, x: int) -> int:
        S = self.S
        return (((S[256 + ((x >> 8) & 0xFF)] + S[x & 0xFF]) & 0xFFFFFFFF)
                ^ S[512 + ((x >> 16) & 0xFF)]) + S[768 + ((x >> 24) & 0xFF)] & 0xFFFFFFFF

    def _enc(self, l: int, r: int) -> tuple[int, int]:
        """返回 (lo, hi) = 密文前 4 字节、后 4 字节（小端存储序）。"""
        P, F = self.P, self._f
        l ^= P[0]
        for k in range(1, 17, 2):
            r ^= F(l) ^ P[k]
            r &= 0xFFFFFFFF
            l ^= F(r) ^ P[k + 1]
            l &= 0xFFFFFFFF
        r ^= P[17]
        r &= 0xFFFFFFFF
        l &= 0xFFFFFFFF
        return r, l

    def _dec(self, l: int, r: int) -> tuple[int, int]:
        """输入 (lo, hi) = 密文前 4 字节、后 4 字节，返回明文 (前4B, 后4B)。"""
        P, F = self.P, self._f
        l ^= P[17]
        for k in range(16, 0, -2):
            r ^= F(l) ^ P[k]
            r &= 0xFFFFFFFF
            l ^= F(r) ^ P[k - 1]
            l &= 0xFFFFFFFF
        r ^= P[0]
        r &= 0xFFFFFFFF
        l &= 0xFFFFFFFF
        return r, l

    def decrypt_ecb(self, data: bytes) -> bytes:
        """FUN_008197e0：先解末尾交叠 8B（余数块），再从头解整 8B 块。"""
        m = bytearray(data)
        n = len(m)
        if n & 7:
            lo, hi = struct.unpack_from("<II", m, n - 8)
            a, b = self._dec(lo, hi)
            struct.pack_into("<II", m, n - 8, a, b)
        for off in range(0, n & ~7, 8):
            lo, hi = struct.unpack_from("<II", m, off)
            a, b = self._dec(lo, hi)
            struct.pack_into("<II", m, off, a, b)
        return bytes(m)


class RC4:
    """FUN_009351f0(KSA) + FUN_00934e30(PRGA) 移植；状态跨帧连续。"""

    def __init__(self, key: bytes):
        self.i = 0
        self.j = 0
        S = list(range(256))
        j = 0
        for i in range(256):
            j = (j + S[i] + key[i % len(key)]) & 0xFF
            S[i], S[j] = S[j], S[i]
        self.S = S

    def crypt(self, data: bytes) -> bytes:
        S = self.S
        i, j = self.i, self.j
        out = bytearray(len(data))
        for k, b in enumerate(data):
            i = (i + 1) & 0xFF
            j = (j + S[i]) & 0xFF
            S[i], S[j] = S[j], S[i]
            out[k] = S[(S[i] + S[j]) & 0xFF] ^ b
        self.i, self.j = i, j
        return bytes(out)


def cred_hash_seed(blob: bytes) -> int:
    """FUN_007a1370/7a1590 的凭据哈希（197 轮 rotl19 变异），返回 DAT_00ad0f0c 种子。

    注意：此移植与实测帧内 seq（0x86081521）不完全吻合，种子派生尚有未确定细节
    （可能与哈希输入缓冲区/非对齐读取有关）。仅作参考，不参与解密。
    """
    d = list(struct.unpack("<23I", blob[:92]))
    u = 0x69A5274B
    p = 0
    for _ in range(0xC5):
        x = d[p] ^ u
        u = (u + (((x << 19) | (x >> 13)) & 0xFFFFFFFF)) & 0xFFFFFFFF
        d[p] = u
        p += 1
        if p == 23:
            p = 0
    return ((d[17] >> 24) | ((d[18] & 0xFFFFFF) << 8)) | 1


def c2s_rc4_key(blob93: bytes) -> bytes:
    """RC4 密钥 = 93B 凭据块 + MD5 大写 hex(凭据块) 32B ASCII = 125B。"""
    import hashlib
    assert len(blob93) == 93
    return blob93 + hashlib.md5(blob93).hexdigest().upper().encode("ascii")


def parse_login_plain(pt: bytes) -> dict:
    """登录帧明文（111B，不含长度头）→ 凭据字段。"""
    blob = pt[18:18 + 93]
    account = blob[0:51].split(b"\0", 1)[0]
    password = blob[51:71].split(b"\0", 1)[0]
    session = struct.unpack_from("<I", blob, 71)[0]
    return {"cmd": struct.unpack_from("<H", pt, 0)[0],
            "seq": struct.unpack_from("<I", pt, 14)[0],
            "blob": blob, "account": account, "password": password,
            "session": session}


def decode_c2s_stream(stream: bytes, exe_path: str = r"D:\software\dhsh\shdata\Game.exe",
                      ) -> list[dict]:
    """离线解密整条 c2s 重组流。返回逐帧 dict（含 ok/错误说明）。"""
    import hashlib
    frames = []
    pos = 0
    rc4 = None
    bf = None
    idx = 0
    while pos + 2 <= len(stream):
        ln = struct.unpack_from("<H", stream, pos)[0]
        if ln < 1 or pos + 2 + ln > len(stream):
            frames.append({"index": idx, "offset": pos, "ok": False,
                           "error": f"长度异常/截断 len={ln} 剩余={len(stream) - pos - 2}"})
            break
        ct = stream[pos + 2: pos + 2 + ln]
        rec = {"index": idx, "offset": pos, "wire_len": ln}
        if idx == 0:
            bf = Blowfish(_load_static(exe_path)[2], exe_path)
            pt = bf.decrypt_ecb(ct)
            info = parse_login_plain(pt)
            rec.update(ok=info["cmd"] == 0x4004, kind="login-blowfish",
                       plain=pt, account=info["account"], password=info["password"],
                       seq=info["seq"],
                       session=info["session"],
                       seed_experimental=cred_hash_seed(info["blob"]))
            rc4 = RC4(c2s_rc4_key(info["blob"]))
        else:
            body, trailer = ct[:-4], ct[-4:]
            pt = rc4.crypt(body)
            md5 = hashlib.md5(body[: min(len(body), 0x100)]).hexdigest().upper()
            rec.update(ok=True, kind="rc4",
                       plain=pt, cmd=struct.unpack_from("<H", pt, 0)[0],
                       seq=struct.unpack_from("<I", pt, 14)[0],
                       trailer=trailer.decode("ascii", "replace"),
                       trailer_ok=trailer == md5[:4].encode("ascii"))
        frames.append(rec)
        pos += 2 + ln
        idx += 1
    return frames
