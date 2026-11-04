"""Locate Sony SDK functions in the stripped YKTK3 ELF (SLPM_658.53, SDK 3.0) -> src/urban/sdk_functions.inc.

Replaces tools/find_sdk_functions.py, which needs Shadow of the Colossus' ELF with symbols as its reference.
Sources, in order:
  analyzer: the SDK names ps2xAnalyzer gave the previous YKTK3 project (recomp/inputs/analyzer.toml: stubs and
            entry points, "name@0xADDRESS").
  syscall:  libkernel syscall wrappers (addiu $v1, N; syscall; jr $ra) named by syscall number; they override the
            analyzer when both name the same function.
  wrapper:  public entry points that only forward to an internal function the analyzer named (checked by hand in the
            disassembly, see WRAPPERS).
  rpc:      remaining libcdvd entry points named by the command they send through sceSifCallRpc on the N-command
            (client used by sceCdRead) or S-command (client used by sceCdGetError) server, as find_sdk_functions.py.

usage: python tools/yktk3_sdk_functions.py --elf <SLPM_658.53> --analyzer <analyzer.toml>
"""
import argparse
import os
import re
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SDK_NAME = re.compile(r"^(i?sce|_sce)")
TEXT_START, TEXT_END = 0x100000, 0x56F1B8

SYSCALL_WRAPPERS = {
    0x6B: "sceSifStopDma",
    0x76: "sceSifDmaStat", -0x76: "isceSifDmaStat",
    0x77: "sceSifSetDma", -0x77: "isceSifSetDma",
    0x78: "sceSifSetDChain", -0x78: "isceSifSetDChain",
    0x79: "sceSifSetReg",
    0x7A: "sceSifGetReg",
}
# address -> (name, what it forwards to)
WRAPPERS = {
    0x5569A8: ("sceSifLoadModule", "_sceSifLoadModule(path, args, argp, &local, 0)"),
    0x5569C8: ("sceSifLoadStartModule", "_sceSifLoadModule(path, args, argp, result, 0)"),
    0x556740: ("sceSifLoadModuleBuffer", "_sceSifLoadModuleBuffer(ptr, args, argp, &local)"),
    0x556760: ("sceSifLoadStartModuleBuffer", "_sceSifLoadModuleBuffer(ptr, args, argp, result)"),
    0x556AF0: ("sceSifLoadElfPart", "_sceSifLoadElfPart(path, secname, data, 1)"),
    0x556B10: ("sceSifLoadElf", "_sceSifLoadElfPart(path, \"all\", data, 1)"),
    0x49CCB0: ("sceCdSearchFile", "sceCdLayerSearchFile(fp, name, 0)"),
}
CD_NCMD = {5: "sceCdSeek", 6: "sceCdStandby", 7: "sceCdStop", 8: "sceCdPause"}
CD_SCMD = {1: "sceCdReadClock", 3: "sceCdGetDiskType", 4: "sceCdGetError", 5: "sceCdTrayReq", 0x16: "sceCdBreak"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--analyzer", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "src", "urban", "sdk_functions.inc"))
    args = ap.parse_args()

    data = open(args.elf, "rb").read()
    phoff, phnum = struct.unpack_from("<I", data, 0x1C)[0], struct.unpack_from("<H", data, 0x2C)[0]
    segments = [struct.unpack_from("<6I", data, phoff + i * 32) for i in range(phnum)]

    def word(address):
        for _, offset, vaddr, _, filesz, _ in segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from("<I", data, offset + address - vaddr)[0]
        return 0

    found = {}  # address -> (name, method, score)
    by_name = {}

    def accept(address, name, method, score):
        old = by_name.get(name)
        if old is not None and old != address:
            if found[old][1] != "analyzer":
                return
            del found[old]
        found.pop(address, None)
        found[address] = (name, method, score)
        by_name[name] = address

    text = open(args.analyzer, encoding="utf-8").read()
    for name, address in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)[@,](0x[0-9A-Fa-f]{6,8})"', text):
        if SDK_NAME.match(name) and name not in by_name:
            accept(int(address, 16), name, "analyzer", 0.0)

    for address in range(TEXT_START, TEXT_END - 8, 4):
        w = word(address)
        if (w >> 16) == 0x2403 and word(address + 4) == 0x0000000C and word(address + 8) == 0x03E00008:
            number = struct.unpack("<h", struct.pack("<H", w & 0xFFFF))[0]
            if number in SYSCALL_WRAPPERS:
                accept(address, SYSCALL_WRAPPERS[number], "syscall", 1.0)

    for address, (name, _) in WRAPPERS.items():
        accept(address, name, "wrapper", 1.0)

    # pass rpc (find_sdk_functions.py pass 4): libcdvd functions by the command they send.
    if all(n in by_name for n in ("sceSifCallRpc", "sceCdRead", "sceCdGetError")):
        call_rpc = by_name["sceSifCallRpc"]

        def rpc_calls(func, end):
            regs, calls = {0: 0}, []
            for addr in range(func, end, 4):
                w = word(addr)
                op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
                simm = imm - 0x10000 if imm & 0x8000 else imm
                if op == 0x0F:
                    regs[rt] = imm << 16
                elif op == 0x09 and rs in regs:
                    regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
                elif op == 0x0D and rs in regs:
                    regs[rt] = regs[rs] | imm
                elif op == 0x03:
                    slot = word(addr + 4)
                    if slot >> 26 == 0x09 and ((slot >> 21) & 31) in regs:
                        s_imm = slot & 0xFFFF
                        regs[(slot >> 16) & 31] = (regs[(slot >> 21) & 31] + (s_imm - 0x10000 if s_imm & 0x8000 else s_imm)) & 0xFFFFFFFF
                    if (w & 0x03FFFFFF) << 2 == call_rpc & 0x0FFFFFFF:
                        calls.append((regs.get(4), regs.get(5)))
            return calls

        # Function bounds: prologues (addiu $sp, $sp, -N) in the libcdvd area plus the located entry points.
        lo = min(by_name["sceCdRead"], by_name["sceCdGetError"]) - 0x2000
        hi = max(by_name["sceCdRead"], by_name["sceCdGetError"]) + 0x2000
        starts = sorted({a for a in range(lo, hi, 4) if (word(a) & 0xFFFF8000) == 0x27BD8000} | {a for a in found if lo <= a < hi})
        next_start = dict(zip(starts, starts[1:]))
        ncmd = {c for c, _ in rpc_calls(by_name["sceCdRead"], next_start[by_name["sceCdRead"]])}
        scmd = {c for c, _ in rpc_calls(by_name["sceCdGetError"], next_start[by_name["sceCdGetError"]])}
        for func in starts:
            if func in found or func not in next_start:
                continue
            for client, command in rpc_calls(func, next_start[func]):
                name = (CD_NCMD if client in ncmd else CD_SCMD if client in scmd else {}).get(command)
                if name and name not in by_name:
                    accept(func, name, "rpc", 1.0)
                    break

    rows = sorted((a, n, m, s) for a, (n, m, s) in found.items())
    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("// Generated by tools/yktk3_sdk_functions.py - do not edit.\n")
        out.write("// X(guest address, SDK name, match method, similarity)\n")
        for address, name, method, score in rows:
            out.write(f'X(0x{address:08X}u, {name}, "{method}", {score:.2f}f)\n')
    counts = {}
    for _, _, method, _ in rows:
        counts[method] = counts.get(method, 0) + 1
    print(f"{len(rows)} SDK functions -> {os.path.relpath(args.out, ROOT)} {counts}")


if __name__ == "__main__":
    main()
