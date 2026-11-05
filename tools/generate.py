"""Regenerate the recompiled C++ from your own YKTK3 (SLPM-65853) disc image.

1. Finds the ISO (game/*.iso, URBAN_ISO or --iso), extracts SLPM_658.53 and checks its hash.
2. Runs ps2_recomp with recomp/yktk3.toml into a staging directory.
3. Copies only changed files into generated/, so an incremental build recompiles only what changed.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

import cheat_code

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BOOT_ELF = "SLPM_658.53"
BOOT_SHA256 = "3697202792fa559312ae1e0f89dd91514d99034cd754f8a608ec50dbee3fd4e0"


def iso_read_file(iso_path, name):
    """Read one file from the ISO9660 root directory."""
    with open(iso_path, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            return None
        root = pvd[156:156 + 34]
        lba, size = struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0]
        f.seek(lba * 2048)
        data = f.read(size)
        i = 0
        while i < len(data):
            length = data[i]
            if length == 0:
                i = (i // 2048 + 1) * 2048
                continue
            entry = data[i:i + length]
            entry_name = entry[33:33 + entry[32]].decode("ascii", "replace").split(";")[0]
            if entry_name.upper() == name.upper():
                f.seek(struct.unpack("<I", entry[2:6])[0] * 2048)
                return f.read(struct.unpack("<I", entry[10:14])[0])
            i += length
    return None


def find_iso(arg):
    if arg:
        return arg
    if os.environ.get("URBAN_ISO"):
        return os.environ["URBAN_ISO"]
    for path in glob.glob(os.path.join(ROOT, "game", "*.iso")):
        cnf = iso_read_file(path, "SYSTEM.CNF")
        if cnf and BOOT_ELF.encode() in cnf:
            return path
    sys.exit("No disc image found. Put your own YKTK3 (SLPM-65853) image in game/ or pass --iso <path>.")


def find_recompiler():
    if os.name != "nt":  # Linux (docs/LINUX.md): only the one built from runtime/ps2xRecomp
        candidate = os.path.join(ROOT, "build", "linux", "runtime", "ps2xRecomp", "ps2_recomp")
        if os.path.exists(candidate):
            return candidate
        sys.exit("ps2_recomp not found (python build.py generate builds it: -DURBAN_BUILD_RECOMP=ON, target ps2_recomp)")
    candidates = [
        os.path.join(ROOT, "build", "port", "runtime", "ps2xRecomp", "ps2_recomp.exe"),
        os.path.join(ROOT, "tools", "ps2_recomp.exe"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    sys.exit("ps2_recomp.exe not found (tools/ps2_recomp.exe, or configure with -DURBAN_BUILD_RECOMP=ON and build ps2_recomp)")


def sync_tree(src, dst):
    os.makedirs(dst, exist_ok=True)
    changed = removed = 0
    wanted = set(os.listdir(src))
    for name in wanted:
        a, b = os.path.join(src, name), os.path.join(dst, name)
        new = open(a, "rb").read()
        if os.path.exists(b) and open(b, "rb").read() == new:
            continue
        open(b, "wb").write(new)
        changed += 1
    for name in os.listdir(dst):
        if name not in wanted and os.path.isfile(os.path.join(dst, name)):
            os.remove(os.path.join(dst, name))
            removed += 1
    print(f"generated sources: {len(wanted)} total, {changed} written, {removed} removed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso")
    args = parser.parse_args()

    iso = find_iso(args.iso)
    print(f"disc image: {iso}")
    elf = iso_read_file(iso, BOOT_ELF)
    if elf is None:
        sys.exit(f"{BOOT_ELF} not found on the disc")
    digest = hashlib.sha256(elf).hexdigest()
    if digest != BOOT_SHA256:
        sys.exit(f"{BOOT_ELF} hash mismatch ({digest}); expected {BOOT_SHA256}")
    build = os.path.join(ROOT, "build")
    os.makedirs(build, exist_ok=True)
    with open(os.path.join(build, BOOT_ELF), "wb") as f:
        f.write(elf)

    # Same config as recomp/yktk3.toml, but written to a staging dir so unchanged files keep their timestamps.
    staging = os.path.join(build, "generated_staging")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)
    toml = open(os.path.join(ROOT, "recomp", "yktk3.toml"), encoding="utf-8").read()
    toml = re.sub(r'(?m)^input\s*=.*$', f'input = "{os.path.join(build, BOOT_ELF).replace(os.sep, "/")}"', toml)
    toml = re.sub(r'(?m)^output\s*=.*$', f'output = "{staging.replace(os.sep, "/")}"', toml)
    # ghidra_output is written relative to recomp/; the staged copy lives elsewhere, so make it absolute.
    def absolute_ghidra(m):
        path = os.path.normpath(os.path.join(ROOT, "recomp", m.group(1)))
        return f'ghidra_output = "{path.replace(os.sep, "/")}"' if os.path.exists(path) else '# ghidra map missing: ' + m.group(1)
    toml = re.sub(r'(?m)^ghidra_output\s*=\s*"([^"]+)"', absolute_ghidra, toml)
    # Jump-table cases as internal entry points (tools/ghidra_functions.py writes the list).
    entries = []
    entries_path = os.path.join(ROOT, "recomp", "yktk3_entry_points.txt")
    if os.path.exists(entries_path):
        entries = [line.strip() for line in open(entries_path, encoding="utf-8") if line.strip() and not line.startswith("#")]
    # Native code cheats (tools/cheat_code.py): the code the .pnach files of the game's mods folder rewrite or add,
    # compiled with the game (URBAN_CHEATS_DIR: another folder).
    bin_dir = os.path.join(build, "port" if os.name == "nt" else "linux", "bin")
    mods = os.environ.get("URBAN_CHEATS_DIR") or os.path.join(bin_dir, "mods")
    # The cheats one by one (tools/cheat_groups.py): mods/기능별, one .pnach per cheat function, compiled with the rest.
    if os.path.isdir(mods):
        import cheat_groups
        cheat_groups.split(mods, os.path.join(build, BOOT_ELF), os.path.join(ROOT, "recomp", "yktk3_functions.csv"))
    cheat_path = os.path.join(build, "cheat_code.txt")
    cheat_entries = cheat_code.build(os.path.join(build, BOOT_ELF), mods, os.path.join(ROOT, "recomp", "yktk3_functions.csv"),
                                     cheat_path, os.path.join(bin_dir, "cheats_native.txt") if os.path.isdir(bin_dir) else None)
    entries += [f"0x{e:08X}" for e in cheat_entries]
    general = "\nentry_points = [" + ", ".join(f'"{e}"' for e in entries) + "]" if entries else ""
    general += f'\ncheat_code = "{cheat_path.replace(os.sep, "/")}"'
    toml = re.sub(r'(?m)^\[general\]\s*$', lambda m: m.group(0) + general, toml, count=1)
    staged_toml = os.path.join(build, "yktk3.staging.toml")
    open(staged_toml, "w", encoding="utf-8").write(toml)

    cmd = [find_recompiler(), staged_toml]
    print("+", " ".join(cmd), flush=True)
    with open(os.path.join(build, "recomp.log"), "w", encoding="utf-8") as log:
        r = subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit(f"ps2_recomp failed ({r.returncode}); see build/recomp.log")
    sync_tree(staging, os.path.join(ROOT, "generated"))


if __name__ == "__main__":
    main()
