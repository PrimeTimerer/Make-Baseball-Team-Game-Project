"""YKTK3 Korean translation: builds build/port/bin/korean/DATA.CVM/... (read by the port as a disc overlay).

What it makes:
  FONT/FONT_KANJI.SVR  the game's kanji font with Hangul syllables drawn into free kanji cells (layer B), so a Hangul
                       syllable is written in the game's text as the Shift-JIS code of the kanji whose cell it took;
  MESSAGE/<name>.PRS   message tables (TBB1, PRS-compressed) with the strings of text/<name>.json translated;
  IMAGE/<name>.PAC     packages that start with a TBB1 table (text/<name>_PAC.json), rebuilt in the table's space.

Translations (text/*.json): {"index": "Korean text with the game's control codes kept as {hex}"}.
Font facts (2026-11-13): FONT_KANJI.SVR is 1024x1024 PSMT4 stored for a PSMCT32 upload; each 4-bit pixel holds two
2-bit glyph layers (A and B below); cells are 22x20 (46 x 51); global cell = JIS index - 736 for kanji from 亜, layer B
first. Hangul is rendered by HangulGlyphs.exe (Malgun Gothic Bold 21 px, 4 levels).

usage: py -3 tools/korean/kobuild.py [output folder, default build/port/bin/korean]
"""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import cvm  # noqa: E402
import gsmem  # noqa: E402
import mes  # noqa: E402

# Level of glyph layer A and layer B for each 4-bit pixel value (layer B = the SVR's own palette).
LAYER_A = [0, 1, 3, 2, 0, 1, 3, 0, 1, 0, 1, 3, 3, 2, 2, 2]
LAYER_B = [0, 1, 2, 3, 1, 2, 3, 2, 3, 3, 0, 1, 0, 1, 0, 2]
PIXEL = {(LAYER_A[i], LAYER_B[i]): i for i in range(16)}
CSC = r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
PLAN_PATH = "/IMAGE/PLAN_TBB.PRS"
PLAN_KEY = "@PLAN_TBB"
PLAN_FIRST, PLAN_RECORD, PLAN_FIELD = 0x860, 0x88, 0x28


def jis_index(code):
    b1, b2 = code >> 8, code & 0xFF
    s1 = b1 - 0x40 if b1 >= 0xE0 else b1
    ku = (s1 - 0x81) * 2 + 1
    if b2 >= 0x9F:
        ku += 1
        ten = b2 - 0x9E
    else:
        ten = b2 - 0x40 + 1 - (1 if b2 >= 0x80 else 0)
    return (ku - 1) * 94 + (ten - 1)


def is_hangul(c):
    return 0xAC00 <= ord(c) <= 0xD7A3


def assign(syllables, free):
    """Hangul syllable -> free kanji code. Kept in hangul_map.json so a syllable keeps its cell from build to build
    (quick saves hold the font in video memory, so moving cells would garble them); new syllables take the next free
    cells, layer B first. KO_LAYER_A_FIRST=1 fills layer A first and is not saved (layer test)."""
    pool = (free["A"] + free["B"]) if os.environ.get("KO_LAYER_A_FIRST") == "1" else (free["B"] + free["A"])
    if os.environ.get("KO_LAYER_A_FIRST") == "1":
        return dict(zip(sorted(syllables), pool))
    path = os.path.join(HERE, "hangul_map.json")
    m = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
    taken = set(m.values())
    spare = [c for c in pool if c not in taken]
    added = 0
    for s in sorted(syllables):
        if s not in m:
            if not spare:
                raise SystemExit("no free kanji cell left for %s (%d syllables mapped)" % (s, len(m)))
            m[s] = spare.pop(0)
            added += 1
    if added:
        json.dump(dict(sorted(m.items())), open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=0)
    return {s: m[s] for s in syllables}


def encode(template, m):
    """Text with {hex} control codes -> game bytes (Hangul via the map, everything else as Shift-JIS)."""
    out = bytearray()
    for part in re.split(r"(\{[0-9a-f]+\})", template):
        if not part:
            continue
        if re.fullmatch(r"\{(?:[0-9a-f]{2})+\}", part):
            out += bytes.fromhex(part[1:-1])
            continue
        for c in part.replace("\u00b7", "\u30fb"):
            if is_hangul(c):
                code = m[c]
                out += bytes((code >> 8, code & 0xFF))
            else:
                out += c.encode("cp932")
    return bytes(out)


def render(chars, work):
    exe = os.path.join(work, "HangulGlyphs.exe")
    src = os.path.join(HERE, "HangulGlyphs.cs")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.check_call([CSC, "-nologo", "-optimize+", "-out:" + exe, "-r:System.Drawing.dll", src])
    txt = os.path.join(work, "glyph_chars.txt")
    out = os.path.join(work, "glyphs.bin")
    open(txt, "w", encoding="utf-8").write("".join(chars))
    subprocess.check_call([exe, txt, out, "Malgun Gothic", "21"], stdout=subprocess.DEVNULL)
    return open(out, "rb").read()


def build_font(m, files, out_path, work):
    svr = cvm.read("/FONT/FONT_KANJI.SVR", files)
    rows = gsmem.unswizzle_t4_from_ct32(svr[0x60:], 1024, 1024)
    chars = sorted(m)
    g = render(chars, work)
    for k, ch in enumerate(chars):
        cell = jis_index(m[ch]) - 736
        layer_a = cell >= 46 * 51
        if layer_a:
            cell -= 46 * 51
        assert 0 <= cell < 46 * 51, (ch, hex(m[ch]))
        cx, cy = cell % 46, cell // 46
        for y in range(20):
            row = rows[cy * 20 + y]
            for x in range(22):
                old = row[cx * 22 + x]
                level = g[k * 440 + y * 22 + x]
                row[cx * 22 + x] = PIXEL[(level, LAYER_B[old])] if layer_a else PIXEL[(LAYER_A[old], level)]
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "wb").write(svr[:0x60] + gsmem.swizzle_t4_to_ct32(rows, 1024, 1024))


def translated(strings, translations, m):
    new = list(strings)
    for k, t in translations.items():
        b = encode(t, m)
        if strings[int(k)].endswith(b"\0") and not b.endswith(b"\0"):
            b += b"\0"
        new[int(k)] = b
    return new


def build_message(name, translations, m, files, out_path):
    d = cvm.prs(cvm.read("/MESSAGE/" + name, files))
    r = mes.rebuild(d, translated(mes.strings(d), translations, m))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "wb").write(cvm.prs_file(r))


def build_pac(path, translations, m, files, out_path):
    d = cvm.read(path, files)
    (base, ents), = mes.parse(d)
    tend = base + 4 + 8 * len(ents)
    rel = 0 if ents[0][0] == tend else base
    limit = rel + ents[-1][0] + ents[-1][1]
    while d[limit] == 0:
        limit += 1
    r = mes.rebuild(d[:limit], translated(mes.strings(d[:limit]), translations, m))
    if len(r) > limit:
        raise SystemExit("%s: table %d bytes > %d available" % (path, len(r), limit))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "wb").write(r + bytes(limit - len(r)) + d[limit:])


def load(path, files):
    d = cvm.read(path, files)
    return (cvm.prs(d), True) if d[:4] == b"PRSH" else (d, False)


def table_span(d, off):
    """(end of the last string, end of the zero padding after it) of the TBB1 table at off."""
    (base, ents), = mes.parse(d[off:])
    tend = base + 4 + 8 * len(ents)
    rel = 0 if ents[0][0] == tend else base
    end = off + rel + ents[-1][0] + ents[-1][1]
    limit = end
    while limit < len(d) and d[limit] == 0:
        limit += 1
    return end, limit


def prs_blocks(d):
    """Offsets of the PRSH blocks of a file (a package of blocks and raw resources at 16-byte steps)."""
    out = []
    i = d.find(b"PRSH")
    while i >= 0:
        if i % 16 == 0:
            out.append(i)
        i = d.find(b"PRSH", i + 4)
    return out


def find_twins(files, work):
    """Copies of the message tables outside MESSAGE, in IMAGE/ and SET/ files. Copies sit in a whole file, inside a
    compressed block of a package (SET/*.FPS: blocks at fixed offsets, read by offset), or inside a raw .PAC.
    Returns [[path, block offset or -1 for a raw file, table offset inside the (decompressed) data, message name]].
    Cached in build/korean_work/twins2.json."""
    cache = os.path.join(work, "twins3.json")
    if os.path.exists(cache):
        return json.load(open(cache))
    signatures = {}
    for p in files:
        if p.startswith("/MESSAGE/") and p.endswith("_MES.PRS"):
            d, _ = load(p, files)
            if d[:4] == b"TBB1":
                signatures[tuple(mes.strings(d))] = p.split("/")[-1][:-4]

    def tables_in(data):
        found = []
        i = data.find(b"TBB1")
        while i >= 0:
            try:
                name = signatures.get(tuple(mes.strings(data[i:])))
                if name:
                    found.append((i, name))
            except Exception:
                pass
            i = data.find(b"TBB1", i + 4)
        return found

    plan_raw = cvm.prs(cvm.read(PLAN_PATH, files))
    twins = []
    for p in sorted(files):
        if p.startswith("/MESSAGE/") or not (p.startswith("/IMAGE/") or p.startswith("/SET/")):
            continue
        if files[p][1] > 8_000_000:
            continue
        raw = cvm.read(p, files)
        blocks = prs_blocks(raw)
        if blocks:
            for b in blocks:
                try:
                    data = cvm.prs(raw[b:])
                except Exception:
                    continue
                if data == plan_raw:
                    twins.append([p, b, 0, PLAN_KEY])
                for off, name in tables_in(data):
                    twins.append([p, b, off, name])
        else:
            for off, name in tables_in(raw):
                twins.append([p, -1, off, name])
    json.dump(twins, open(cache, "w"), indent=0)
    return twins


def rebuild_tables(data, items, texts, m, where, warnings):
    """Translated tables inside data (in the space each table has up to the next non-zero byte; a table that fills
    the data to its end may grow)."""
    data = bytearray(data)
    for off, name in sorted(items, reverse=True):
        end, limit = table_span(data, off)
        S = mes.strings(bytes(data[off:end]))
        r = mes.rebuild(bytes(data[off:end]), translated(S, texts[name], m))
        if limit >= len(data):
            data = data[:off] + bytearray(r)
        elif off + len(r) <= limit:
            data[off:limit] = r + bytes(limit - off - len(r))
        else:
            warnings.append("%s table @%#x (%s): %d bytes > %d free, left in Japanese" % (where, off, name, len(r), limit - off))
    return bytes(data)


def apply_plan_names(data, names, m, warnings):
    """Plan names in PLAN_TBB (TBL1 at 0x850: 157 records of 0x88 bytes from 0x860, name = first 0x28 bytes)."""
    data = bytearray(data)
    assert data[PLAN_FIRST:PLAN_FIRST + 22] == "チャリティオークション".encode("cp932"), "unexpected PLAN_TBB layout"
    for i, ko in names.items():
        b = encode(ko, m)
        if len(b) > PLAN_FIELD - 1:
            warnings.append("plan %d %s: %d bytes > %d, left in Japanese" % (i, ko, len(b), PLAN_FIELD - 1))
            continue
        o = PLAN_FIRST + i * PLAN_RECORD
        data[o:o + PLAN_FIELD] = b + bytes(PLAN_FIELD - len(b))
    return bytes(data)


def build_twins(twins, texts, plans, m, files, out, warnings):
    by_file = {}
    for p, block, off, name in twins:
        if name in texts or (name == PLAN_KEY and plans and p == PLAN_PATH):
            by_file.setdefault(p, {}).setdefault(block, []).append((off, name))
    for p, blocks in sorted(by_file.items()):
        raw = bytearray(cvm.read(p, files))
        if -1 in blocks:
            raw = bytearray(rebuild_tables(raw, blocks[-1], texts, m, p, warnings))
        else:
            starts = prs_blocks(bytes(raw))
            # last block first so the earlier offsets stay valid
            for b in sorted(blocks, reverse=True):
                data = cvm.prs(bytes(raw[b:]))
                items = blocks[b]
                if any(name == PLAN_KEY for _, name in items):
                    data = apply_plan_names(data, plans, m, warnings)
                    items = [it for it in items if it[1] != PLAN_KEY]
                new = cvm.prs_file(rebuild_tables(data, items, texts, m, "%s block @%#x" % (p, b), warnings))
                later = [x for x in starts if x > b]
                if not later:  # the last block: the file may grow
                    raw = raw[:b] + bytearray(new) + bytes(-len(new) % 16)
                elif b + len(new) <= later[0]:
                    raw[b:later[0]] = new + bytes(later[0] - b - len(new))
                else:
                    warnings.append("%s block @%#x: %d bytes > %d before the next block, left in Japanese" % (p, b, len(new), later[0] - b))
        target = os.path.join(out, "DATA.CVM", *p.strip("/").split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        open(target, "wb").write(bytes(raw))


def load_plans():
    """Korean plan names by plan number, from tools/cheat_names_ko.tsv (rows: plan, number, name, effect)."""
    plans = {}
    path = os.path.join(ROOT, "tools", "cheat_names_ko.tsv")
    for line in open(path, encoding="utf-8"):
        cols = line.rstrip("\n").split("\t")
        if len(cols) >= 3 and cols[0] == "plan" and cols[1].isdigit():
            plans[int(cols[1])] = cols[2]
    return plans


def elf_bytes():
    """A reader of the game executable by address (build/SLPM_658.53, else the file on the disc)."""
    import struct
    path = os.path.join(ROOT, "build", "SLPM_658.53")
    d = open(path, "rb").read() if os.path.exists(path) else cvm.iso_file("SLPM_658.53")
    phoff, phnum = struct.unpack_from("<I", d, 0x1C)[0], struct.unpack_from("<H", d, 0x2C)[0]
    segs = [struct.unpack_from("<8I", d, phoff + 32 * i) for i in range(phnum)]

    def mem(addr, n):
        for kind, off, vaddr, _, filesz, _, _, _ in segs:
            if kind == 1 and vaddr <= addr < vaddr + filesz:
                return d[off + addr - vaddr: off + addr - vaddr + n]
        return b""
    return mem


def ram_patches(m, warnings):
    """tools/korean/ram/*.json (name tables in the executable) -> [(address, bytes)] for the game's memory."""
    out = []
    mem = elf_bytes()
    folder = os.path.join(HERE, "ram")
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if not name.endswith(".json"):
            continue
        t = json.load(open(os.path.join(folder, name), encoding="utf-8"))
        base, stride = int(t["base"], 16), int(t["stride"])
        for i, (ja, ko) in enumerate(zip(t["ja"], t["ko"])):
            addr = base + i * stride
            have = mem(addr, stride).split(b"\0")[0]
            if have != ja.encode("cp932"):
                warnings.append("%s %d: the executable has %r at %#x, not %s; skipped" % (name, i, have, addr, ja))
                continue
            b = encode(ko, m)
            if len(b) > stride - 1:
                warnings.append("%s %d %s: %d bytes > %d; skipped" % (name, i, ko, len(b), stride - 1))
                continue
            out.append((addr, b + bytes(stride - len(b)), mem(addr, stride)))
    return out


def exe_entries():
    """The executable's strings with a translation: [(address, japanese, max, korean)] from exe/source.json and exe/ko*.json."""
    folder = os.path.join(HERE, "exe")
    if not os.path.exists(os.path.join(folder, "source.json")):
        return []
    source = json.load(open(os.path.join(folder, "source.json"), encoding="utf-8"))
    ko = {}
    for name in sorted(os.listdir(folder)):
        if name.startswith("ko") and name.endswith(".json"):
            ko.update(json.load(open(os.path.join(folder, name), encoding="utf-8")))
    return [(int(a, 16), source[a]["ja"], source[a]["max"], k, source[a].get("pack", False))
            for a, k in sorted(ko.items()) if a in source and k]


def data_words():
    """[(address, value)] of every aligned word in the executable's data sections (pointer tables)."""
    import struct
    path = os.path.join(ROOT, "build", "SLPM_658.53")
    d = open(path, "rb").read() if os.path.exists(path) else cvm.iso_file("SLPM_658.53")
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(shnum)]
    names = secs[shstrndx][4]
    out = []
    for sec in secs:
        name = d[names + sec[0]:].split(b"\0")[0]
        if name in (b".data", b".rodata", b".sdata") and sec[5]:
            n = sec[5] // 4
            out += [(sec[3] + 4 * i, v) for i, v in enumerate(struct.unpack_from("<%dI" % n, d, sec[4]))]
    return out


def packed_patches(m, warnings):
    """Areas of strings that only pointer tables reach (exe_extract.PACKED; no code reference, checked when they were
    chosen): the Korean strings packed one after another inside the area, and the pointers moved to them, so a name
    may be longer than its old slot. [(address, korean bytes, japanese bytes)]"""
    import struct
    import exe_extract
    mem = elf_bytes()
    entries = {e[0]: e for e in exe_entries() if e[4]}
    source = json.load(open(os.path.join(HERE, "exe", "source.json"), encoding="utf-8"))
    words = data_words()
    out = []
    for lo, hi in exe_extract.PACKED:
        names = sorted(int(a, 16) for a, v in source.items() if v.get("pack") and lo <= int(a, 16) < hi)
        pointers = [(at, v) for at, v in words if lo <= v < hi]
        stray = [hex(v) for _, v in pointers if v not in names]
        missing = [hex(a) for a in names if a not in entries]
        if stray or missing:
            warnings.append("packed %x-%x: pointers into a string %s, untranslated %s; left in Japanese" %
                            (lo, hi, stray, missing))
            continue
        packed, moved = bytearray(), {}
        for a in names:
            moved[a] = lo + len(packed)
            packed += encode(entries[a][3], m) + b"\0"
        if len(packed) > hi - lo:
            warnings.append("packed %x-%x: %d bytes > %d; left in Japanese" % (lo, hi, len(packed), hi - lo))
            continue
        packed += bytes(hi - lo - len(packed))
        out.append((lo, bytes(packed), mem(lo, hi - lo)))
        out += [(at, struct.pack("<I", moved[v]), struct.pack("<I", v)) for at, v in pointers]
    return out


def exe_patches(m, warnings):
    """[(address, korean bytes, japanese bytes)]: each string is written over the Japanese one, NUL-filled to cover it."""
    import exe_check
    out = []
    mem = elf_bytes()
    for addr, ja, mx, ko, pack in exe_entries():
        if pack:
            continue  # packed_patches
        j = ja.encode("cp932")
        if mem(addr, len(j) + 1) != j + b"\0":
            warnings.append("exe %06x: the executable has other bytes than %s; skipped" % (addr, ja))
            continue
        if exe_check.FORMAT.findall(ko) != exe_check.FORMAT.findall(ja):
            warnings.append("exe %06x %s: format specifiers differ from %s; skipped" % (addr, ko, ja))
            continue
        b = encode(ko, m)
        if len(b) > mx:
            warnings.append("exe %06x %s: %d bytes > %d; skipped" % (addr, ko, len(b), mx))
            continue
        size = max(len(j), len(b)) + 1
        out.append((addr, b + bytes(size - len(b)), mem(addr, size)))
    return out


def plan_ram_lines(plans, m, files, warnings):
    """The plan table in memory: [0x5CD51C] -> "TBL1" (the second table of PLAN_TBB), records of 0x88 bytes from +0x10,
    the name in the first 0x28 bytes. Each line replaces one name while it still holds the Japanese one."""
    raw = cvm.prs(cvm.read(PLAN_PATH, files))
    tbl = PLAN_FIRST - 0x10
    assert raw[tbl:tbl + 4] == b"TBL1"
    lines = []
    for i, ko in sorted(plans.items()):
        b = encode(ko, m)
        if len(b) > PLAN_FIELD - 1:
            warnings.append("plan %d %s: %d bytes > %d, left in Japanese" % (i, ko, len(b), PLAN_FIELD - 1))
            continue
        off = 0x10 + i * PLAN_RECORD
        ja = raw[tbl + off: tbl + off + PLAN_FIELD]
        # the Japanese field by its FNV-1a 64 hash only: the file carries no Japanese text (korean.cpp keeps the bytes
        # it sees in memory)
        lines.append("@005cd51c %s %x %s #%016x" % (b"TBL1".hex(), off, (b + bytes(PLAN_FIELD - len(b))).hex(), fnv1a64(ja)))
    return lines


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def build_textures(out, work, warnings):
    """Korean textures: tools/korean/tex/textures.json drawn over the textures the game dumped (PCSX2 texture replacement,
    names = texture hashes) into <out>/textures/replacements, which the port loads (main.cpp). The dumps come from a run
    with PS2X_PCSX2_TEXTURE_DIR=build/korean_work/texdump and PS2X_PCSX2_TEXTURE_DUMP=1. Returns the count."""
    spec = os.path.join(HERE, "tex", "textures.json")
    dumps = os.path.join(work, "texdump", "dumps")
    if not os.path.exists(spec) or not os.path.isdir(dumps):
        return 0
    exe = os.path.join(work, "tex", "TexTool.exe")
    src = os.path.join(HERE, "TexTool.cs")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.check_call([CSC, "-nologo", "-optimize+", "-r:System.Drawing.dll", "-r:System.Web.Extensions.dll",
                               "-out:" + exe, src])
    target = os.path.join(out, "textures", "replacements")
    if os.path.isdir(target):
        shutil.rmtree(target)
    result = subprocess.run([exe, "render", spec, dumps, target], capture_output=True, text=True)
    for line in result.stderr.splitlines():
        warnings.append("textures: " + line)
    return len([f for f in os.listdir(target) if f.endswith(".png")]) if os.path.isdir(target) else 0


def ram_texts():
    folder = os.path.join(HERE, "ram")
    out = []
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if name.endswith(".json"):
            out += json.load(open(os.path.join(folder, name), encoding="utf-8"))["ko"]
    return out + [e[3] for e in exe_entries()]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "port", "bin", "korean")
    work = os.path.join(ROOT, "build", "korean_work")
    os.makedirs(work, exist_ok=True)
    files = cvm.listing()
    import check  # noqa: E402  (tools/korean/check.py)
    texts = {}
    skipped = []
    for name in sorted(os.listdir(os.path.join(HERE, "text"))):
        if not name.endswith(".json"):
            continue
        key = name[:-5]
        if not key.endswith("_PAC") and os.path.exists(os.path.join(HERE, "source", name)):
            errors, _ = check.check(key)
            if errors:
                skipped.append("%s (%d problems)" % (key, len(errors)))
                continue
        texts[key] = json.load(open(os.path.join(HERE, "text", name), encoding="utf-8"))
    for s in skipped:
        print("skipped, check.py finds problems: " + s)
    plans = load_plans()
    extra = list(plans.values()) + ram_texts()
    syll = {c for tr in texts.values() for t in tr.values() for c in t if is_hangul(c)}
    syll |= {c for t in extra for c in t if is_hangul(c)}
    m = assign(syll, json.load(open(os.path.join(HERE, "free_codes.json"))))
    build_font(m, files, os.path.join(out, "DATA.CVM", "FONT", "FONT_KANJI.SVR"), work)
    for key, tr in texts.items():
        if key.endswith("_PAC"):
            path = "/IMAGE/%s.PAC" % key[:-4]
            build_pac(path, tr, m, files, os.path.join(out, "DATA.CVM", "IMAGE", key[:-4] + ".PAC"))
        else:
            build_message(key + ".PRS", tr, m, files, os.path.join(out, "DATA.CVM", "MESSAGE", key + ".PRS"))
    warnings = []
    twins = find_twins(files, work)
    build_twins(twins, texts, plans, m, files, out, warnings)
    tables = ram_patches(m, warnings)
    strings = exe_patches(m, warnings) + packed_patches(m, warnings)
    patches, end = [], 0
    for addr, b, ja in sorted(tables + strings):
        if addr < end:
            warnings.append("memory patch %06x overlaps the one before it; skipped" % addr)
            continue
        patches.append((addr, b, ja))
        end = addr + len(b)
    plan_lines = plan_ram_lines(plans, m, files, warnings)
    with open(os.path.join(out, "RAM_PATCH.TXT"), "w", encoding="ascii") as f:
        # Korean bytes only: the game's Japanese ones are read from memory when the port starts (korean.cpp)
        f.write("# <address> <korean>: name tables and strings of the executable, written over the game memory\n")
        for addr, b, ja in patches:
            f.write("%08x %s\n" % (addr, b.hex()))
        f.write("# @<pointer> <magic> <offset> <korean> #<fnv1a64 of the Japanese field>: the plan table the game loads "
                "([0x5CD51C] -> TBL1)\n")
        for line in plan_lines:
            f.write(line + "\n")
    textures = build_textures(out, work, warnings)
    for w in warnings:
        print("warning: " + w)
    copies = sum(1 for t in twins if t[3] in texts)
    print("Korean: %d syllables, %d translated files, %d copies elsewhere, %d plan names, %d memory patches "
          "(%d executable strings), %d textures -> %s" % (len(syll), len(texts), copies, len(plans), len(patches), len(strings),
                                                    textures, out))


if __name__ == "__main__":
    main()
