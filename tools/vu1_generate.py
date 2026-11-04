"""Compiles the VU1 microcode images into generated/vu1 (the next build links them), without rebuilding what did not
change. python build.py generate runs it after the game code.

The images come from:
- recomp/vu1_images.txt, in the repository: where the pieces of each image lie in the game's ELF, no game bytes. They
  are rebuilt from build/SLPM_658.53 (tools/generate.py extracts it from your disc image and checks it), and each must
  match its hash.
- build/vu1_capture, if it exists: images captured while playing (PS2X_VU1_CAPTURE=build/vu1_capture, one
  vu1_<hash>.bin per microcode image). --record adds the new ones to recomp/vu1_images.txt.

python tools/vu1_generate.py [--capture build/vu1_capture] [--record]
   - leaves out the images in EXCLUDED (they stay interpreted),
   - runs vu1_recomp (configure with -DURBAN_BUILD_TOOLS=ON, target vu1_recomp) into build/vu1_staging,
   - copies into generated/vu1 only the files whose content changed and removes the ones that are gone, so the build
     recompiles only those (the 655 files of a full set take ~10 minutes).
"""
import argparse
import filecmp
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, "build", "port" if os.name == "nt" else "linux")
RECOMP = os.path.join(BUILD, "runtime", "ps2xTest", "vu1_recomp.exe" if os.name == "nt" else "vu1_recomp")
IMAGES = os.path.join(ROOT, "recomp", "vu1_images.txt")
CODE_SIZE = 16 * 1024  # VU1 micro memory; the runtime keys an image by the FNV-1a 64 of all of it
ZERO = bytes(8)
# Compiled code that differs from the interpreter (PS2X_VU1_VERIFY=obs): the w lane of stored vectors gets a VI value
# where the interpreter keeps the memory's (2026-11-04, the opening of a COM vs COM match). Interpreted, the match still
# runs at ~58 of 60 fields/s.
EXCLUDED = {"7a19a174a1858c8e", "821ecaddc3221d92", "aa9deea297816605"}
HEADER = """\
# VU1 microcode images for tools/vu1_generate.py, rebuilt from your own SLPM_658.53: no game bytes, only where the
# pieces of each image lie in the ELF. One image per line: the FNV-1a 64 hash of its 16 KB, then its pieces in order,
# <ELF file offset>+<length> or z+<length> (zeros), in hexadecimal. --record adds the images of build/vu1_capture.
"""


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def section(elf, name):
    """File offsets [start, end) of an ELF section."""
    shoff = struct.unpack_from("<I", elf, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", elf, 0x30)
    headers = [struct.unpack_from("<10I", elf, shoff + i * 40) for i in range(shnum)]
    names = headers[shstrndx][4]
    for h in headers:
        if elf[names + h[0]:elf.index(b"\0", names + h[0])] == name:
            return h[4], h[4] + h[5]
    sys.exit(f"no {name.decode()} section in the ELF")


def rebuild(elf, pieces):
    image = bytearray()
    for piece in pieces:
        start, length = piece.split("+")
        length = int(length, 16)
        image += bytes(length) if start == "z" else elf[int(start, 16):int(start, 16) + length]
    return bytes(image)


class PieceFinder:
    """Finds the pieces of an image in .vutext, where the game's MPG uploads come from: longest match first."""
    KEY = 32

    def __init__(self, elf):
        self.elf = elf
        lo, hi = section(elf, b".vutext")
        self.long, self.word = {}, {}
        for o in range(lo, hi - 7, 8):
            self.word.setdefault(elf[o:o + 8], []).append(o)
            if o + self.KEY <= hi:
                self.long.setdefault(elf[o:o + self.KEY], []).append(o)

    def pieces(self, image):
        pieces, pos = [], 0
        while pos < CODE_SIZE:
            if image[pos:pos + 8] == ZERO:
                end = pos
                while end < CODE_SIZE and image[end:end + 8] == ZERO:
                    end += 8
                pieces.append(f"z+{end - pos:x}")
                pos = end
                continue
            best, best_len = None, 0
            # What is left of an older upload can be shorter than KEY: then one instruction pair is the key.
            for index, key in ((self.long, self.KEY), (self.word, 8)):
                for start in index.get(image[pos:pos + key], ())[:4000]:
                    n = key
                    while pos + n < CODE_SIZE and image[pos + n:pos + n + 8] == self.elf[start + n:start + n + 8]:
                        n += 8
                    if n > best_len:
                        best, best_len = start, n
                if best is not None:
                    break
            if best is None:
                return None
            pieces.append(f"{best:x}+{best_len:x}")
            pos += best_len
        return pieces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=os.path.join(ROOT, "build", "vu1_capture"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "SLPM_658.53"))
    ap.add_argument("--record", action="store_true", help="add the captured images to recomp/vu1_images.txt")
    args = ap.parse_args()
    if not os.path.exists(RECOMP):
        sys.exit(f"{RECOMP} missing: python build.py configure -DURBAN_BUILD_TOOLS=ON, then build the vu1_recomp target")

    recipes = {}
    if os.path.exists(IMAGES):
        for line in open(IMAGES, encoding="utf-8"):
            fields = line.split()
            if fields and not fields[0].startswith("#"):
                recipes[fields[0]] = fields[1:]
    captured = {}
    if os.path.isdir(args.capture):
        for name in os.listdir(args.capture):
            if name.startswith("vu1_") and name.endswith(".bin"):
                captured[name[4:-4]] = open(os.path.join(args.capture, name), "rb").read()
    elf = None
    if recipes or args.record:
        if not os.path.exists(args.elf):
            sys.exit(f"{args.elf} missing: python build.py generate extracts it from your disc image")
        elf = open(args.elf, "rb").read()

    images = {}
    for name, pieces in recipes.items():
        image = rebuild(elf, pieces)
        if len(image) != CODE_SIZE or f"{fnv1a64(image):016x}" != name:
            sys.exit(f"VU1 image {name} does not rebuild from {args.elf}: is it the SLPM-65853 ELF?")
        images[name] = image
    if args.record:
        new = sorted(set(captured) - set(recipes))
        finder = PieceFinder(elf) if new else None
        missing = []
        for name in new:
            pieces = finder.pieces(captured[name])
            if pieces is None or rebuild(elf, pieces) != captured[name]:
                missing.append(name)
            else:
                recipes[name] = pieces
        with open(IMAGES, "w", encoding="utf-8", newline="\n") as out:
            out.write(HEADER)
            for name in sorted(recipes):
                out.write(" ".join([name, *recipes[name]]) + "\n")
        print(f"recomp/vu1_images.txt: {len(new) - len(missing)} images added, {len(recipes)} in all")
        if missing:
            print(f"  not found in the ELF (compiled here from the capture only): {' '.join(missing)}")
    from_list = len(images)
    images.update(captured)
    if not images:
        sys.exit("no VU1 images: recomp/vu1_images.txt is missing and nothing is captured")

    selected = os.path.join(ROOT, "build", "vu1_images")
    staging = os.path.join(ROOT, "build", "vu1_staging")
    out = os.path.join(ROOT, "generated", "vu1")
    for path in (selected, staging):
        shutil.rmtree(path, ignore_errors=True)
        os.makedirs(path)
    for name, image in images.items():
        if name not in EXCLUDED:
            with open(os.path.join(selected, f"vu1_{name}.bin"), "wb") as f:
                f.write(image)
    r = subprocess.run([RECOMP, selected, staging], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"vu1_recomp failed ({r.returncode}):\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    os.makedirs(out, exist_ok=True)
    new = set(os.listdir(staging))
    written = removed = 0
    for name in sorted(new):
        target = os.path.join(out, name)
        if not os.path.exists(target) or not filecmp.cmp(os.path.join(staging, name), target, shallow=False):
            shutil.copy2(os.path.join(staging, name), target)
            written += 1
    for name in os.listdir(out):
        if name not in new:
            os.remove(os.path.join(out, name))
            removed += 1
    excluded = len(EXCLUDED & set(images))
    print(f"{len(images)} images ({from_list} rebuilt from recomp/vu1_images.txt, {len(images) - from_list} more captured; "
          f"{excluded} left out) -> {len(new)} files in generated/vu1: {written} written, {removed} removed")


if __name__ == "__main__":
    main()
