"""Read files from YKTK3's DATA.CVM (inside the ISO) and SEGA PRS decompression."""
import struct

import os
ISO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "game", "YAKYUTSUKU3.iso")
CVM_LBA = 920455
CVM_BASE = CVM_LBA * 2048 + 0x1800  # inner ISO9660 image after the 0x1800-byte CVMH header


def _iso():
    return open(ISO, "rb")


def listing():
    """{path: (lba, size)} for every file inside DATA.CVM (lba relative to the inner image)."""
    out = {}
    with _iso() as f:
        def rd(lba, n):
            f.seek(CVM_BASE + lba * 2048)
            return f.read(n)

        def walk(lba, size, path):
            data = rd(lba, size)
            i = 0
            while i < len(data):
                l = data[i]
                if l == 0:
                    i = (i // 2048 + 1) * 2048
                    continue
                rec = data[i:i + l]
                e_lba, e_size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
                flags, nl = rec[25], rec[32]
                name = rec[33:33 + nl]
                if name not in (b"\x00", b"\x01"):
                    n = name.decode("ascii", "replace").split(";")[0]
                    if flags & 2:
                        walk(e_lba, e_size, path + n + "/")
                    else:
                        out[path + n] = (e_lba, e_size)
                i += l

        pvd = rd(16, 2048)
        root = pvd[156:156 + 34]
        walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "/")
    return out


def read(path, files=None):
    files = files or listing()
    lba, size = files[path]
    with _iso() as f:
        f.seek(CVM_BASE + lba * 2048)
        return f.read(size)


def prs(src):
    if src[:4] == b"PRSH":
        hdr = int.from_bytes(src[4:8], "little")
        src = src[hdr:]
    """SEGA PRS decompression."""
    out = bytearray()
    i = 0
    bits = 0
    ctrl = 0

    def bit():
        nonlocal i, bits, ctrl
        if bits == 0:
            ctrl = src[i]
            i += 1
            bits = 8
        b = ctrl & 1
        ctrl >>= 1
        bits -= 1
        return b

    while True:
        if bit():
            out.append(src[i])
            i += 1
            continue
        if bit():
            data = src[i] | (src[i + 1] << 8)
            i += 2
            if data == 0:
                break
            offset = (data >> 3) - 0x2000
            size = data & 7
            if size == 0:
                size = src[i] + 1
                i += 1
            else:
                size += 2
        else:
            size = bit() << 1
            size |= bit()
            size += 2
            offset = src[i] - 256
            i += 1
        start = len(out) + offset
        for k in range(size):
            out.append(out[start + k])
    return bytes(out)


def prs_compress(data):
    """SEGA PRS compression (greedy LZ77 with hash chains)."""
    out = bytearray()
    state = {"ctrl": -1, "bits": 8}

    def bit(b):
        if state["bits"] == 8:
            state["ctrl"] = len(out)
            out.append(0)
            state["bits"] = 0
        if b:
            out[state["ctrl"]] |= 1 << state["bits"]
        state["bits"] += 1

    n = len(data)
    heads = {}
    i = 0
    while i < n:
        best_len, best_dist = 0, 0
        if i + 2 < n:
            key = data[i:i + 3]
            cand = heads.get(key)
            if cand:
                for j in reversed(cand[-64:]):
                    dist = i - j
                    if dist > 0x1FFF:
                        break
                    l = 0
                    while i + l < n and l < 256 and data[j + l] == data[i + l]:
                        l += 1
                    if l > best_len:
                        best_len, best_dist = l, dist
                        if l == 256:
                            break
        if best_len < 3 and i + 1 < n:
            # short matches of 2 within 256
            for dist in range(1, min(256, i) + 1):
                if data[i - dist] == data[i] and data[i - dist + 1] == data[i + 1]:
                    l = 2
                    while i + l < n and l < 5 and data[i - dist + l] == data[i + l]:
                        l += 1
                    if l > best_len:
                        best_len, best_dist = l, dist
                    break
        use = 0
        if best_len >= 2 and best_dist <= 256 and best_len <= 5:
            bit(0); bit(0)
            s = best_len - 2
            bit((s >> 1) & 1); bit(s & 1)
            out.append((256 - best_dist) & 0xFF)
            use = best_len
        elif best_len >= 3:
            bit(0); bit(1)
            off = (0x2000 - best_dist) & 0x1FFF
            if best_len <= 9:
                v = (off << 3) | (best_len - 2)
                out += bytes((v & 0xFF, v >> 8))
            else:
                v = off << 3
                out += bytes((v & 0xFF, v >> 8, best_len - 1))
            use = best_len
        else:
            bit(1)
            out.append(data[i])
            use = 1
        for k in range(i, min(i + use, n - 2)):
            heads.setdefault(data[k:k + 3], []).append(k)
        i += use
    bit(0); bit(1)
    out += b"\0\0"
    return bytes(out)


def prs_file(data):
    """PRSH header (as on the disc) + compressed data (optimal parsing: smaller than the disc's own files)."""
    comp = prs_compress_best(data) if len(data) <= 128 * 1024 else prs_compress(data)
    hdr = bytearray(0x40)
    hdr[0:4] = b"PRSH"
    hdr[4:8] = (0x40).to_bytes(4, "little")
    hdr[8:12] = len(comp).to_bytes(4, "little")
    hdr[12:16] = len(data).to_bytes(4, "little")
    return bytes(hdr) + comp


def prs_compress_best(data, depth=48):
    """Smaller PRS output by optimal parsing (cost in bits; slow, for blocks that must fit a fixed slot)."""
    n = len(data)
    heads = {}
    matches = [None] * n
    for i in range(n):
        cands = []
        if i + 1 < n:
            key = data[i:i + 2]
            lst = heads.get(key)
            if lst:
                for j in reversed(lst[-depth:]):
                    dist = i - j
                    if dist > 0x1FFF:
                        break
                    l = 2
                    while i + l < n and l < 256 and data[j + l] == data[i + l]:
                        l += 1
                    cands.append((dist, l))
            heads.setdefault(key, []).append(i)
        matches[i] = cands
    cost = [0] * (n + 1)
    choice = [(0, 1)] * n
    for i in range(n - 1, -1, -1):
        best = 9 + cost[i + 1]
        ch = (0, 1)
        for dist, L in matches[i]:
            if dist <= 256:
                for l in range(2, min(5, L) + 1):
                    c = 12 + cost[i + l]
                    if c < best:
                        best, ch = c, (dist, l)
            if L >= 3:
                for l in range(3, min(9, L) + 1):
                    c = 18 + cost[i + l]
                    if c < best:
                        best, ch = c, (dist, l)
                for l in range(10, L + 1):
                    c = 26 + cost[i + l]
                    if c < best:
                        best, ch = c, (dist, l)
        cost[i] = best
        choice[i] = ch
    out = bytearray()
    state = {"ctrl": -1, "bits": 8}

    def bit(b):
        if state["bits"] == 8:
            state["ctrl"] = len(out)
            out.append(0)
            state["bits"] = 0
        if b:
            out[state["ctrl"]] |= 1 << state["bits"]
        state["bits"] += 1

    i = 0
    while i < n:
        dist, l = choice[i]
        if dist == 0:
            bit(1)
            out.append(data[i])
        elif dist <= 256 and l <= 5:
            bit(0); bit(0)
            s = l - 2
            bit((s >> 1) & 1); bit(s & 1)
            out.append((256 - dist) & 0xFF)
        else:
            bit(0); bit(1)
            off = (0x2000 - dist) & 0x1FFF
            if l <= 9:
                v = (off << 3) | (l - 2)
                out += bytes((v & 0xFF, v >> 8))
            else:
                v = off << 3
                out += bytes((v & 0xFF, v >> 8, l - 1))
        i += l
    bit(0); bit(1)
    out += b"\0\0"
    return bytes(out)


def prs_file_best(data):
    comp = prs_compress_best(data)
    hdr = bytearray(0x40)
    hdr[0:4] = b"PRSH"
    hdr[4:8] = (0x40).to_bytes(4, "little")
    hdr[8:12] = len(comp).to_bytes(4, "little")
    hdr[12:16] = len(data).to_bytes(4, "little")
    return bytes(hdr) + comp


def iso_file(name):
    """A file from the disc's root directory (outside DATA.CVM), e.g. the executable SLPM_658.53."""
    with _iso() as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        f.seek(lba * 2048)
        data = f.read(size)
        i = 0
        while i < len(data):
            l = data[i]
            if l == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + l]
            n = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
            if n.upper() == name.upper():
                f.seek(struct.unpack_from("<I", rec, 2)[0] * 2048)
                return f.read(struct.unpack_from("<I", rec, 10)[0])
            i += l
    raise FileNotFoundError(name)
