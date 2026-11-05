"""Native code cheats: what the .pnach files of the game's "mods" folder do to the game's code, for ps2_recomp
(tools/generate.py runs it before the recompiler).

PCSX2 runs whatever is in memory, so a cheat that rewrites instructions changes the game there. Here the game code is
compiled ahead of time and rewriting instructions in memory would change nothing. So ps2_recomp compiles, at each place
a cheat rewrites, every version of the code the cheats can leave there, and the compiled code runs the version that
matches the words in memory at that moment (cheat on/off, conditions). Code a cheat writes into free memory and jumps
to is compiled as functions of its own.

The writes follow PCSX2 1.6.0: word/short/byte lines go through the EE memory map (main RAM and its mirrors; any other
address, e.g. a CodeBreaker "E..." line written with the word type, is ignored), extended lines are CodeBreaker codes
(0/1/2 writes, 4 serial writes, D/E conditionals; the other types only change data at run time).

Output (build/cheat_code.txt, read by ps2_recomp through cheat_code in the staged TOML):
  R <start> <end> <words>;<words>...   a region of game code and the word lists the cheats can leave in it
  C <start> <end> <words>              a cheat routine: code a cheat writes into free memory and jumps to
and the jump targets inside game functions (resume entry points) for the recompiler's entry_points, plus
<mods>/../cheats_native.txt for the launcher: the cheat files compiled in (sha256, name, code or data).

usage: python tools/cheat_code.py --elf build/SLPM_658.53 --mods build/port/bin/mods --functions recomp/yktk3_functions.csv
"""
import argparse
import hashlib
import itertools
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
TEXT = (0x100000, 0x56F1B8)
MAX_COMBINED_FILES = 6


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        phoff, phnum = struct.unpack_from("<I", self.data, 0x1C)[0], struct.unpack_from("<H", self.data, 0x2C)[0]
        self.segments = [struct.unpack_from("<8I", self.data, phoff + i * 32) for i in range(phnum)]

    def byte(self, address):
        for kind, offset, vaddr, _, filesz, _, _, _ in self.segments:
            if kind == 1 and vaddr <= address < vaddr + filesz:
                return self.data[offset + address - vaddr]
        return 0  # .bss and the free memory below the ELF start zeroed


def ram_address(address):
    """PCSX2's EE memory map for the plain patch types: main RAM and its mirrors, else None (ignored)."""
    if address & 0xFE000000 in (0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xA0000000):
        return address & 0x01FFFFFF
    return None


def parse(path):
    """One pass of a .pnach as PCSX2 1.6.0 applies it: (writes, conditional): byte address -> value; conditional
    holds the writes a D/E code may or may not do, as a list of {address: value}."""
    writes, conditional = {}, []
    lines = []
    for raw in open(path, encoding="utf-8-sig", errors="replace"):
        line = raw.split("//")[0].strip()
        if not line.lower().startswith("patch"):
            continue
        fields = [f.strip() for f in line.split("=", 1)[1].split(",")]
        if len(fields) < 5 or fields[1].lower() != "ee":
            continue
        try:
            lines.append((fields[3].lower(), int(fields[2], 16), int(fields[4], 16)))
        except ValueError:
            continue

    def put(target, address, value, size):
        for i in range(size):
            target[address + i] = (value >> (8 * i)) & 0xFF

    skip = 0       # lines a D/E code puts under its condition
    group = None
    i = 0
    while i < len(lines):
        kind, address, value = lines[i]
        target = writes
        if skip > 0:
            skip -= 1
            target = group
        sizes = {"byte": 1, "short": 2, "word": 4, "double": 8}
        if kind in sizes:
            physical = ram_address(address)
            if physical is not None:
                put(target, physical, value, sizes[kind])
        elif kind == "extended":
            code = address >> 28
            physical = address & 0x01FFFFFF
            if code in (0, 1, 2):
                put(target, physical, value, (1, 2, 4)[code])
            elif code == 4 and i + 1 < len(lines):  # serial write: count/step in this line, value/increment next
                count, step = (value >> 16) & 0xFFFF, value & 0xFFFF
                start_value, increment = lines[i + 1][1], lines[i + 1][2]
                for n in range(count):
                    put(target, physical + n * step * 4, (start_value + n * increment) & 0xFFFFFFFF, 4)
                i += 1
            elif code in (5, 6) or (code == 3 and (address >> 16) & 0xFFF in (0x040, 0x050)):
                i += 1  # two-line codes that only move data at run time
            elif code == 0xD:
                group = {}
                conditional.append(group)
                skip = 1
            elif code == 0xE:
                group = {}
                conditional.append(group)
                skip = (address >> 16) & 0xFFF  # Ennnvvvv taaaaaaa, as ps2_pnach.cpp and PCSX2 1.6.0
        i += 1
    return writes, conditional


def has_delay_slot(word):
    op = word >> 26
    if op in (2, 3, 4, 5, 6, 7, 20, 21, 22, 23):
        return True
    if op == 0:
        return word & 0x3F in (8, 9)  # jr, jalr
    if op == 1:
        return (word >> 16) & 0x1F in (0, 1, 2, 3, 16, 17, 18, 19)  # bltz, bgez(l), bltzal...
    if op in (16, 17, 18):
        return (word >> 21) & 0x1F == 8  # bc0/bc1/bc2
    return False


def jump_target(address, word):
    if word >> 26 in (2, 3):
        return ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
    return None


def build(elf_path, mods, functions_csv, out_path, manifest_path=None):
    """Writes out_path; returns the resume entry points (jump targets inside game functions)."""
    elf = Elf(elf_path)
    files = sorted(f for f in os.listdir(mods) if f.lower().endswith(".pnach")) if mods and os.path.isdir(mods) else []
    # mods/기능별: the cheats one by one (tools/cheat_groups.py), as "기능별/<file>" like config.ini lists them
    groups = os.path.join(mods, "기능별") if mods else ""
    if groups and os.path.isdir(groups):
        files += sorted("기능별/" + f for f in os.listdir(groups) if f.lower().endswith(".pnach"))
    starts, bounds = set(), []
    for line in open(functions_csv, encoding="utf-8"):
        parts = line.strip().split(",")
        if len(parts) >= 3 and parts[1].startswith("0x"):
            start, end = int(parts[1], 16), int(parts[2], 16)
            starts.add(start)
            bounds.append((start, end))
    bounds.sort()

    def function_of(address):
        lo, hi = 0, len(bounds)
        while lo < hi:
            mid = (lo + hi) // 2
            if bounds[mid][0] <= address:
                lo = mid + 1
            else:
                hi = mid
        if lo and bounds[lo - 1][0] <= address < bounds[lo - 1][1]:
            return bounds[lo - 1]
        return None

    def original(address):
        return int.from_bytes(bytes(elf.byte(address + i) for i in range(4)), "little")

    def word_in(state, address):
        return int.from_bytes(bytes(state.get(address + i, elf.byte(address + i)) for i in range(4)), "little")

    # Each file's possible states: its writes, with or without each conditional group.
    states = {}
    kinds = {}
    for name in files:
        writes, conditional = parse(os.path.join(mods, name))
        variants = [dict(writes)]
        for group in conditional[:4]:
            variants += [{**v, **group} for v in variants]
        states[name] = variants
        touched = {a & ~3 for v in variants for a in v}
        kinds[name] = "code" if any(TEXT[0] <= a < TEXT[1] and any(word_in(v, a) != original(a) for v in variants) for a in touched) else "data"

    # Regions: runs of game code words some file changes, grown so a branch and its delay slot stay together.
    changed = sorted({a & ~3 for name in files for v in states[name] for a in v
                      if TEXT[0] <= a < TEXT[1] and any(word_in(s, a & ~3) != original(a & ~3) for s in states[name])})
    runs = []
    for a in changed:
        if runs and runs[-1][1] == a:
            runs[-1][1] = a + 4
        else:
            runs.append([a, a + 4])
    grown = True
    while grown:
        grown = False
        for run in runs:
            if has_delay_slot(original(run[0] - 4)):
                run[0] -= 4
                grown = True
            last = run[1] - 4
            if any(has_delay_slot(word_in(s, last)) for name in files for s in states[name]) or has_delay_slot(original(last)):
                if not any(r is not run and r[0] <= run[1] < r[1] for r in runs):
                    run[1] += 4
                    grown = True
        runs.sort()
        merged = []
        for run in runs:
            if merged and run[0] <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], run[1])
            else:
                merged.append(run)
        if len(merged) != len(runs):
            grown = True
        runs = merged

    regions, entries, problems = [], set(), []
    for start, end in runs:
        owner = function_of(start)
        if owner is None or end > owner[1]:
            problems.append(f"region 0x{start:08X}-0x{end:08X} is not inside one game function: left out")
            continue
        base = [original(a) for a in range(start, end, 4)]
        touching = [n for n in files if any(any(word_in(s, a) != original(a) for a in range(start, end, 4)) for s in states[n])]
        lists = set()
        for count in range(1, min(len(touching), MAX_COMBINED_FILES) + 1):
            for chosen in itertools.combinations(touching, count):
                for combo in itertools.product(*(states[n] for n in chosen)):
                    merged = {}
                    for state in combo:
                        merged.update(state)
                    words = tuple(word_in(merged, a) for a in range(start, end, 4))
                    if list(words) != base:
                        lists.add(words)
        if lists:
            regions.append((start, end, sorted(lists)))

    # Cheat routines: code written outside the game code that a changed word or another routine jumps to.
    written = {}
    for name in files:
        for state in states[name]:
            for a, v in state.items():
                if not (TEXT[0] <= a < TEXT[1]):
                    written.setdefault(a & ~3, {}).setdefault(name, {})[a] = v
    memory = {}
    conflicts = {}
    for word_address, by_file in written.items():
        values = {word_in(bytes_, word_address) for bytes_ in by_file.values()}
        if len(values) > 1:
            conflicts[word_address] = sorted(by_file)
        memory[word_address] = word_in(next(iter(by_file.values())), word_address)

    def cave_word(address):
        return memory.get(address, original(address))

    queue = [(a, w) for start, end, lists in regions for words in lists for a, w in zip(range(start, end, 4), words)]
    caves = {}
    while queue:
        address, word = queue.pop()
        target = jump_target(address, word)
        if target is None:
            continue
        if TEXT[0] <= target < TEXT[1]:
            if target not in starts:
                entries.add(target)
            continue
        if target not in memory or target in caves:
            continue
        # From the entry to the end of its code: an unconditional jump (j / jr) and its delay slot where the cheat
        # stops writing. Unwritten words in between run as what memory holds (zeros below the ELF: nop), as in PCSX2,
        # e.g. 27 of them before the closing jump of a routine.
        address2, end, unwritten = target, target + 4, 0
        while address2 < target + 0x4000:
            w = cave_word(address2)
            if address2 in memory:
                unwritten, end = 0, address2 + 4
            else:
                unwritten += 1
                if unwritten >= 64:
                    break
            if w >> 26 == 2 or (w >> 26 == 0 and w & 0x3F == 8):
                end = address2 + 8
                if address2 + 8 not in memory:
                    break
                address2 += 8
                continue
            address2 += 4
        caves[target] = end
        queue += [(a, cave_word(a)) for a in range(target, end, 4)]
    cave_list = []
    starts_sorted = sorted(caves)
    for i, start in enumerate(starts_sorted):
        end = caves[start]
        if i + 1 < len(starts_sorted):
            end = min(end, starts_sorted[i + 1])
        cave_list.append((start, end, [cave_word(a) for a in range(start, end, 4)]))
        for a in range(start, end, 4):
            if a in conflicts:  # data written in two ways is fine; code is not
                problems.append(f"0x{a:08X}: cheat files write different code there ({', '.join(conflicts[a])}): the first is compiled")

    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# tools/cheat_code.py: game code the cheats in " + (mods or "-") + " rewrite, and the code they add.\n")
        for start, end, lists in regions:
            out.write(f"R {start:08X} {end:08X} " + ";".join(" ".join(f"{w:08X}" for w in words) for words in lists) + "\n")
        for start, end, words in cave_list:
            out.write(f"C {start:08X} {end:08X} " + " ".join(f"{w:08X}" for w in words) + "\n")
    if manifest_path:
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as out:
            out.write("# Cheat files compiled into yktk3.exe (tools/cheat_code.py): sha256, kind, name\n")
            for name in files:
                digest = hashlib.sha256(open(os.path.join(mods, name), "rb").read()).hexdigest()
                out.write(f"{digest}\t{kinds[name]}\t{name}\n")
    for problem in problems:
        print("  cheat code: " + problem)
    code_files = [n for n in files if kinds[n] == "code"]
    print(f"cheat code: {len(files)} cheat files ({len(code_files)} with code), {len(regions)} code regions, "
          f"{len(cave_list)} cheat routines, {len(entries)} entry points")
    return sorted(entries)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "SLPM_658.53"))
    ap.add_argument("--mods", default=os.path.join(ROOT, "build", "port" if os.name == "nt" else "linux", "bin", "mods"))
    ap.add_argument("--functions", default=os.path.join(ROOT, "recomp", "yktk3_functions.csv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "cheat_code.txt"))
    args = ap.parse_args()
    entries = build(args.elf, args.mods, args.functions, args.out)
    print("entry points: " + " ".join(f"0x{e:08X}" for e in entries))


if __name__ == "__main__":
    main()
