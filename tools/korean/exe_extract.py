"""Japanese strings of the game executable (SLPM_658.53) for the Korean translation -> tools/korean/exe/source.json.

{"<address>": {"ja": text, "max": bytes}}: every NUL-terminated Shift-JIS string of the data sections with real Japanese in
it, except person names (they stay Japanese like the players' names in the saves), tables done elsewhere (tools/korean/ram)
and debug text. "max" is the room for the Korean bytes: the Japanese length, or the rest of the 8-byte block for a string
literal on its own (8-aligned, followed only by its alignment padding). Format strings never grow (sprintf buffers).
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Address ranges left out: [start, end)
SKIP = [
    (0x5E2A38, 0x5E2E70),  # hometown objects (ram/facilities.json)
    (0x67A160, 0x67A190),  # hometown categories (ram/categories.json)
    (0x677310, 0x677480), (0x6777A0, 0x6778E0), (0x677C00, 0x677E30),  # OB players, legends, celebrities
    (0x69A5D0, 0x69AAE0),  # real players
    (0x676900, 0x676958),  # secretaries
    (0x67B6F0, 0x67B700), (0x6E6108, 0x6E6109), (0x6B4AA0, 0x6B4AA1),  # default owner names, a sender
    (0x676AE8, 0x676AE9), (0x676DFF, 0x676E00),  # reserved slot, debug text
    (0x61B7E6, 0x61B7E7), (0x67A600, 0x67A601), (0x682048, 0x682049), (0x682160, 0x682161), (0x683850, 0x683851),
    (0x6A85D6, 0x6A8700), (0x6B28DE, 0x6B28DF), (0x6E3E30, 0x6E3E31),  # data that only looks like text, debug text
    (0x624D3D, 0x624E20),  # play result labels in a .data record table, some overwritten by other fields
    (0x6CF310, 0x6CF600),  # prefectures, 16-byte slots (ram/prefectures.json)
] + [(a, a + 1) for a in (  # data that only looks like text (found by the translators)
    0x5E948C, 0x606174, 0x6152F0, 0x615380, 0x619068, 0x61C374, 0x61CDF4, 0x61F2F4, 0x671C1C, 0x6734A0, 0x67AC9C,
    0x6A2C50, 0x6A579C, 0x6A691C, 0x6A6C5C, 0x6A7294, 0x6A9844, 0x6A9B44, 0x6B0540, 0x6B13E0, 0x6B2B08, 0x6BFAA8,
    0x6177F0, 0x61DEB4, 0x6A5798, 0x6AD141, 0x6AD181, 0x6E6AA8)]

# Areas whose strings are reached only through pointer tables (no code reference): the build packs the Korean strings
# again inside the area and rewrites the pointers, so a name may be longer than its old slot ("pack": true).
PACKED = [(0x676958, 0x676AE8), (0x682858, 0x6829E8)]  # prefecture lists

# Half-width katakana words are told from float constants (0x3E4CCCCD reads as "ﾍﾌL>") by where they are: these tables.
KANA = [(0x678A00, 0x679700), (0x694E30, 0x694E40), (0x6973A0, 0x6979E0), (0x6CDCC0, 0x6CDCD0), (0x6CF1E8, 0x6CF1F8),
        (0x6CF998, 0x6CFAF8), (0x6D0AF8, 0x6D0B20)]
FORMAT = re.compile(rb"%[-+ #0]*\d*(?:\.\d+)?[hlL]?[diouxXeEfgGcsp%]")


def kinds(b):
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if 0x81 <= c <= 0x9F or 0xE0 <= c <= 0xEF:
            if i + 1 >= len(b) or not (0x40 <= b[i + 1] <= 0xFC and b[i + 1] != 0x7F):
                return None
            v = (c << 8) | b[i + 1]
            common = (0x829F <= v <= 0x82F1 or 0x8340 <= v <= 0x8396 or 0x889F <= v <= 0x9872 or 0x8140 <= v <= 0x81AC
                      or 0x824F <= v <= 0x829A)
            out.append("J" if common else "X")
            i += 2
        elif 0xA1 <= c <= 0xDF:
            out.append("h")
            i += 1
        elif c in (9, 10) or 0x20 <= c < 0x7F:
            out.append("a")
            i += 1
        else:
            return None
    return out


def japanese(b, addr):
    k = kinds(b)
    if k is None:
        return False
    j, h, x = k.count("J"), k.count("h"), k.count("X")  # X: a rare kanji, common in data, possible in a name
    n = len(kinds(FORMAT.sub(b"", b).replace(b" ", b"")))  # format specifiers and spaces don't count against the text
    if j >= 2 and j >= 2 * x and j >= 0.3 * n:
        return True
    return x == 0 and any(lo <= addr < hi for lo, hi in KANA) and j + h >= 2 and j + h >= 0.5 * n


def strings(d):
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    names = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = d[names + s[0]:].split(b"\0")[0].decode()
        if not s[3] or name == ".text" or s[1] == 8:
            continue
        base, blob = s[3], d[s[4]:s[4] + s[5]]
        i = 0
        while i < len(blob):
            if blob[i] == 0:
                i += 1
                continue
            j = blob.find(b"\0", i)
            j = len(blob) if j < 0 else j
            # A string right after other data (a float without a NUL before it) starts at a word boundary past bytes
            # that are not text; never inside a run that is text from its first byte.
            start = i if japanese(blob[i:j], base + i) else next(
                (k for k in range(i + 1, j)
                 if (base + k) % 4 == 0 and kinds(blob[i:k]) is None and japanese(blob[k:j], base + k)), None)
            try:
                blob[start:j].decode("cp932") if start is not None else None
            except UnicodeDecodeError:
                start = None  # bytes no Shift-JIS table has: data
            if start is not None and not any(lo <= base + start < hi for lo, hi in SKIP):
                text = blob[start:j]
                end = j
                while end < len(blob) and blob[end] == 0:
                    end += 1
                room, size, addr = end - start, len(text), base + start
                alone = b"%" not in text and addr % 8 == 0 and room == (size + 8) // 8 * 8
                out["%06x" % addr] = {"ja": text.decode("cp932"), "max": room - 1 if alone else size}
                if any(lo <= addr < hi for lo, hi in PACKED):
                    out["%06x" % addr]["pack"] = True
            i = j
    return out


def main():
    d = open(os.path.join(ROOT, "build", "SLPM_658.53"), "rb").read()
    out = strings(d)
    os.makedirs(os.path.join(HERE, "exe"), exist_ok=True)
    path = os.path.join(HERE, "exe", "source.json")
    json.dump(out, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=0)
    print("%d strings, %d characters -> %s" % (len(out), sum(len(v["ja"]) for v in out.values()), path))


if __name__ == "__main__":
    sys.exit(main())
