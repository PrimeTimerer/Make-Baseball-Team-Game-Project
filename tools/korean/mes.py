"""TBB1 message tables: parse, show, rebuild."""
import struct


def parse(d):
    assert d[:4] == b"TBB1"
    ntab = struct.unpack_from("<I", d, 8)[0]
    tables = []
    for t in range(ntab):
        base = struct.unpack_from("<I", d, 0x10 + 4 * t)[0]
        n = struct.unpack_from("<I", d, base)[0]
        ents = [struct.unpack_from("<II", d, base + 4 + 8 * k) for k in range(n)]
        tables.append((base, ents))
    return tables


def strings(d):
    """List of byte strings for every entry (offsets resolved like the game: try file-relative, else table-relative)."""
    out = []
    for base, ents in parse(d):
        if not ents:
            continue
        tend = base + 4 + 8 * len(ents)
        first = ents[0][0]
        rel = 0 if first == tend else base  # observed both layouts
        for off, ln in ents:
            out.append(d[rel + off: rel + off + ln])
    return out


def show(s):
    r = []
    i = 0
    while i < len(s):
        b = s[i]
        if (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xFC):
            r.append(s[i:i + 2].decode("cp932", "replace"))
            i += 2
        elif 0x20 <= b < 0x7F:
            r.append(chr(b))
            i += 1
        else:
            r.append("{%02X}" % b)
            i += 1
    return "".join(r)


def tokens(s):
    """Split a message into ('t', sjis-bytes) text runs and ('c', code-bytes) control codes.
    Controls: 01 0A (newline) or 01 xx yy. Text: SJIS pairs (lead 81-9F, E0-EF) and ASCII 20-7E."""
    out = []
    i = 0
    text = bytearray()

    def flush():
        if text:
            out.append(("t", bytes(text)))
            text.clear()

    while i < len(s):
        b = s[i]
        if b == 0x01:
            flush()
            if i + 1 < len(s) and s[i + 1] == 0x0A:
                out.append(("c", s[i:i + 2]))
                i += 2
            else:
                out.append(("c", s[i:i + 3]))
                i += 3
        elif (0x81 <= b <= 0x9F) or (0xE0 <= b <= 0xEF):
            text += s[i:i + 2]
            i += 2
        elif 0x20 <= b < 0x7F or 0xA1 <= b <= 0xDF:
            text.append(b)
            i += 1
        elif b == 0:
            flush()
            out.append(("z", s[i:i + 1]))
            i += 1
        else:
            flush()
            out.append(("?", s[i:i + 1]))
            i += 1
    flush()
    return out


def show2(s):
    r = []
    for kind, b in tokens(s):
        if kind == "t":
            r.append(b.decode("cp932", "replace"))
        elif kind == "c":
            r.append("{" + b.hex() + "}")
        elif kind == "?":
            r.append("<%02X>" % b[0])
    return "".join(r)


def rebuild(d, new_strings):
    """Same table layout (header, offset convention, padding) with replaced strings."""
    (base, ents), = parse(d)
    n = len(ents)
    assert len(new_strings) == n
    tend = base + 4 + 8 * n
    rel = 0 if ents[0][0] == tend else base
    head = bytearray(d[:base])
    table = bytearray(struct.pack("<I", n))
    body = bytearray()
    pos = tend
    for s in new_strings:
        table += struct.pack("<II", pos - rel, len(s))
        body += s
        pos += len(s)
    out = head + table + body
    # the files are zero-padded to a multiple of 16 bytes
    while len(out) % 16:
        out += bytes(1)
    return bytes(out)
