"""Builds recomp/yktk3_functions.csv, the Ghidra function map that recomp/yktk3.toml feeds to ps2_recomp.

1. Imports and auto-analyzes build/SLPM_658.53 in a headless Ghidra project (language r5900:LE:32:default from the
   ghidra-emotionengine-reloaded extension). Skip with --no-analyze when the project already exists.
2. Exports functions with PS2Recomp's ExportPS2Functions.java. --raw-csv <file> skips 1 and 2 and reads an export
   made earlier (recomp/inputs/functions.csv, the previous YKTK3 project's export, is the one in use).
3. Keeps only real functions (FUN_*; the export also lists ~80k code labels, which must not become entries) and
   drops starts in a branch delay slot plus known bad splits.
4. Adds the SDK entry points the runtime replaces by address (src/urban/sdk_functions.inc: every row located by a
   method other than the analyzer's names, and every name hle_bindings.cpp binds), so each one is a function entry.
5. Adds the function starts Ghidra missed (missed_entry_points): functions reached only through pointers, which
   the runtime cannot enter otherwise.
6. Writes the jump-table cases as internal entry points (jump_table_targets -> recomp/yktk3_entry_points.txt).

Usage: python tools/ghidra_functions.py [--ghidra <ghidra folder>] [--no-analyze] [--raw-csv <export.csv>] [--elf <ELF>]
"""
import argparse
import bisect
import csv
import os
import re
import struct
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
ELF = os.path.join(ROOT, "build", "SLPM_658.53")
OUT = os.path.join(ROOT, "build", "ghidra")
SCRIPTS = os.path.join(ROOT, "runtime", "ps2xRecomp", "tools", "ghidra")
# Ghidra splits that land in the middle of a function (checked by hand). None known for YKTK3 yet.
BAD_STARTS = set()


def headless(ghidra, *args):
    cmd = [os.path.join(ghidra, "support", "analyzeHeadless.bat"), OUT, "YKTK3", *args]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def is_control_transfer(word):
    op, rs, rt, fn = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 63
    return (op in (2, 3) or 4 <= op <= 7 or 0x14 <= op <= 0x17 or (op == 1 and (rt <= 3 or 16 <= rt <= 19)) or
            (op == 0 and fn in (8, 9)) or (op in (0x11, 0x12) and rs == 8))


def is_terminator(word):
    return word >> 26 == 2 or (word >> 26 == 0 and word & 63 == 8) or word == 0x42000018  # j, jr, eret


def missed_entry_points(elf, starts, case_labels):
    """Function starts Ghidra missed (it merges a function into the one before it when that one ends in a tail call,
    and does not follow pointers): addresses after a terminator (j/jr and its delay slot, then nop padding) that open
    a stack frame, or whose address is taken by a lui/addiu pair or stored in data. A data word is not a function
    pointer when it is a jump-table entry: a Ghidra case label, or a neighbouring word points into the same function."""
    shoff, shentsize, shnum, shstrndx = struct.unpack_from("<I", elf, 0x20)[0], *struct.unpack_from("<HHH", elf, 0x2E)
    sections = [list(struct.unpack_from("<6I", elf, shoff + i * shentsize)) for i in range(shnum)]
    names = sections[shstrndx][4]
    for s in sections:
        s[0] = elf[names + s[0]:elf.index(b"\0", names + s[0])].decode()
    text = next(s for s in sections if s[0] == ".text")
    t0, t1 = text[3], text[3] + text[5]
    code = struct.unpack_from(f"<{text[5] // 4}I", elf, text[4])
    word = lambda a: code[(a - t0) >> 2]

    def after_terminator(c):
        if c - 8 < t0:
            return False
        if is_terminator(word(c - 8)):
            return True
        a = c - 4
        while a >= t0 and word(a) == 0:
            a -= 4
        return a != c - 4 and (is_terminator(word(a)) or (a - 4 >= t0 and is_terminator(word(a - 4))))

    def prologue(c):
        return word(c) >> 16 == 0x27BD and word(c) & 0x8000  # addiu sp, sp, -N

    ordered = sorted(starts)
    owner = lambda a: ordered[bisect.bisect_right(ordered, a) - 1] if a >= ordered[0] else None
    found = {c for c in range(t0, t1, 4) if c not in starts and prologue(c) and after_terminator(c)}
    for a in range(t0, t1, 4):  # lui rt, hi ... addiu/ori rt, rt, lo
        w = word(a)
        if w >> 26 != 0x0F:
            continue
        rt = (w >> 16) & 31
        for b in range(a + 4, min(a + 40, t1), 4):
            x = word(b)
            op, rs, xrt, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
            if rs == rt and op in (0x09, 0x0D):
                v = ((w & 0xFFFF) << 16) + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)
                if t0 <= v < t1 and v % 4 == 0 and v not in starts and v not in case_labels and after_terminator(v):
                    found.add(v)
                break
            if xrt == rt and op not in (0x28, 0x29, 0x2B, 0x3F, 0x1F):  # stores leave rt
                break
    for s in sections:
        if s[1] != 1 or not s[2] & 2 or not s[3] or s[0] == ".text" or s[0].startswith(".vutext"):
            continue
        data = struct.unpack_from(f"<{s[5] // 4}I", elf, s[4])
        for i, v in enumerate(data):
            if not (t0 <= v < t1) or v % 4 or v in starts or v in case_labels or v in found or not after_terminator(v):
                continue
            if not any(0 <= j < len(data) and t0 <= data[j] < t1 and data[j] != v and owner(data[j]) == owner(v)
                       for j in (i - 1, i + 1)):
                found.add(v)
    return found


def jump_table_targets(elf, starts, case_labels):
    """Switch cases the dispatcher may be asked to enter: the recompiler turns a jump table it recognises into a switch,
    but a `jr` it does not (YKTK3's sub_0011C338 indexes its second table with a register bounded through another one)
    jumps through the dispatcher, which only enters registered addresses. Every Ghidra case label and every entry of a
    data table whose neighbours point into the same function becomes an internal entry point of its function
    (recomp/yktk3_entry_points.txt, added to the recompiler's entry_points by tools/generate.py). Delay slots are left
    out: execution never starts there."""
    shoff, shentsize, shnum, shstrndx = struct.unpack_from("<I", elf, 0x20)[0], *struct.unpack_from("<HHH", elf, 0x2E)
    sections = [list(struct.unpack_from("<6I", elf, shoff + i * shentsize)) for i in range(shnum)]
    names = sections[shstrndx][4]
    for s in sections:
        s[0] = elf[names + s[0]:elf.index(b"\0", names + s[0])].decode()
    text = next(s for s in sections if s[0] == ".text")
    t0, t1 = text[3], text[3] + text[5]
    code = struct.unpack_from(f"<{text[5] // 4}I", elf, text[4])
    ordered = sorted(starts)
    owner = lambda a: ordered[bisect.bisect_right(ordered, a) - 1] if a >= ordered[0] else None
    targets = {c for c in case_labels if t0 <= c < t1}
    for s in sections:
        if s[1] != 1 or not s[2] & 2 or not s[3] or s[0] == ".text" or s[0].startswith(".vutext"):
            continue
        data = struct.unpack_from(f"<{s[5] // 4}I", elf, s[4])
        for i, v in enumerate(data):
            if t0 <= v < t1 and v % 4 == 0 and any(0 <= j < len(data) and t0 <= data[j] < t1 and data[j] != v and
                                                   owner(data[j]) == owner(v) for j in (i - 1, i + 1)):
                targets.add(v)
    return sorted(t for t in targets if t not in starts and t - 4 >= t0 and not is_control_transfer(code[(t - 4 - t0) >> 2]))


def sdk_entry_points():
    inc = os.path.join(ROOT, "src", "urban", "sdk_functions.inc")
    if not os.path.exists(inc):
        return set()
    bindings = open(os.path.join(ROOT, "src", "urban", "hle_bindings.cpp"), encoding="utf-8").read()
    bound = set(re.findall(r'"([A-Za-z_]\w*)"', bindings.split("kHleNames[] = {", 1)[1].split("};", 1)[0]))
    rows = re.findall(r'X\(0x([0-9A-Fa-f]+)u, (\w+), "(\w+)"', open(inc, encoding="utf-8").read())
    return {int(a, 16) for a, name, method in rows if method != "analyzer" or name in bound}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ghidra", default=os.environ.get("YKTK3_GHIDRA"),
                    help="Ghidra 12.1.3 folder with ghidra-emotionengine-reloaded (only without --raw-csv)")
    ap.add_argument("--no-analyze", action="store_true")
    ap.add_argument("--raw-csv", help="an existing Ghidra export: recomp/inputs/functions.csv is the one in use")
    ap.add_argument("--elf", default=ELF)
    args = ap.parse_args()
    raw_csv = args.raw_csv
    if not raw_csv and not args.ghidra:
        ap.error("--raw-csv recomp/inputs/functions.csv, or a Ghidra folder (--ghidra / YKTK3_GHIDRA)")
    if not raw_csv:
        os.makedirs(OUT, exist_ok=True)
        raw_csv = os.path.join(OUT, "ghidra_functions.csv")
        if not args.no_analyze:
            headless(args.ghidra, "-import", args.elf, "-processor", "r5900:LE:32:default", "-overwrite",
                     "-analysisTimeoutPerFile", "3600")
        headless(args.ghidra, "-process", os.path.basename(args.elf), "-noanalysis", "-readOnly", "-scriptPath", SCRIPTS,
                 "-postScript", "ExportPS2Functions.java", os.path.join(OUT, "ghidra_export.toml"), raw_csv)

    elf = open(args.elf, "rb").read()
    phoff, phnum = struct.unpack_from("<I", elf, 0x1C)[0], struct.unpack_from("<H", elf, 0x2C)[0]
    segments = [struct.unpack_from("<6I", elf, phoff + i * 32) for i in range(phnum)]

    def word_at(address):
        for _, offset, vaddr, _, filesz, _ in segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from("<I", elf, offset + address - vaddr)[0]
        return None

    rows = list(csv.reader(open(raw_csv, newline="")))
    def is_dropped(start):
        previous = word_at(start - 4)
        return start in BAD_STARTS or (previous is not None and is_control_transfer(previous))

    bodies = {}
    for row in rows[1:]:
        if row and row[0].startswith("FUN_") and not is_dropped(int(row[1], 0)):
            bodies[int(row[1], 0)] = int(row[2], 0)
    dropped = sorted(int(row[1], 0) for row in rows[1:] if row and row[0].startswith("FUN_") and is_dropped(int(row[1], 0)))
    added = sorted(sdk_entry_points() - set(bodies))
    case_labels = {int(row[1], 0) for row in rows[1:] if row and row[0].startswith("caseD_")}
    missed = missed_entry_points(elf, set(bodies) | set(added), case_labels)
    starts = sorted(set(bodies) | set(added) | missed)
    kept = [rows[0]]
    for start in starts:
        # Ghidra's End is the highest address of the body; a function with a detached chunk (shared tail code far
        # away) would otherwise span ~100 KB and be emitted again for every such function (C1002 in MSVC). Its Size
        # skips holes inside the body, so cut at the next function start instead.
        # Holes after the body (code reached only by jumps, padding) stay with the function, as in the
        # recompiler's own discovery; otherwise they would be covered by nothing.
        following = bisect.bisect_right(starts, start)
        end = bodies.get(start)
        if following < len(starts):
            next_start = starts[following]
            end = next_start if end is None or next_start - start <= 0x20000 else min(end, next_start)
        elif end is None:
            continue
        # Same names as the recompiler's own discovery, so generated file names stay stable.
        kept.append([f"sub_{start:08X}", f"0x{start:08X}", f"0x{end:08X}", str(end - start)])
    out = os.path.join(ROOT, "recomp", "yktk3_functions.csv")
    csv.writer(open(out, "w", newline="")).writerows(kept)
    print(f"{out}: {len(kept) - 1} functions ({len(added)} SDK entry points, {len(missed)} missed starts added), "
          f"dropped {len(dropped)} delay-slot starts")
    cases = jump_table_targets(elf, set(starts), case_labels)
    entries = os.path.join(ROOT, "recomp", "yktk3_entry_points.txt")
    with open(entries, "w", newline="\n") as f:
        f.write("# Internal entry points (jump-table cases), written by tools/ghidra_functions.py - do not edit.\n")
        f.writelines(f"0x{t:08X}\n" for t in cases)
    print(f"{entries}: {len(cases)} jump-table cases")


if __name__ == "__main__":
    main()
