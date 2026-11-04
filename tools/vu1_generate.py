"""Compiles the captured VU1 microcode into generated/vu1 (the next build links it), without rebuilding what did not
change.

1. Capture while playing: PS2X_VU1_CAPTURE=build/vu1_capture (one vu1_<hash>.bin per microcode image, entries.txt).
2. python tools/vu1_generate.py [--capture build/vu1_capture]
   - leaves out the images in EXCLUDED (they stay interpreted),
   - runs vu1_recomp (configure with -DURBAN_BUILD_TOOLS=ON, target vu1_recomp) into build/vu1_staging,
   - copies into generated/vu1 only the files whose content changed and removes the ones that are gone, so the build
     recompiles only those (the 655 files of a full set take ~10 minutes).
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
RECOMP = os.path.join(ROOT, "build", "port", "runtime", "ps2xTest", "vu1_recomp.exe" if os.name == "nt" else "vu1_recomp")
# Compiled code that differs from the interpreter (PS2X_VU1_VERIFY=obs): the w lane of stored vectors gets a VI value
# where the interpreter keeps the memory's (2026-11-04, the opening of a COM vs COM match). Interpreted, the match still
# runs at ~58 of 60 fields/s.
EXCLUDED = {"7a19a174a1858c8e", "821ecaddc3221d92", "aa9deea297816605"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", default=os.path.join(ROOT, "build", "vu1_capture"))
    args = ap.parse_args()
    if not os.path.exists(RECOMP):
        sys.exit(f"{RECOMP} missing: python build.py configure -DURBAN_BUILD_TOOLS=ON, then build the vu1_recomp target")
    filtered = os.path.join(ROOT, "build", "vu1_capture_filtered")
    staging = os.path.join(ROOT, "build", "vu1_staging")
    out = os.path.join(ROOT, "generated", "vu1")
    for path in (filtered, staging):
        shutil.rmtree(path, ignore_errors=True)
        os.makedirs(path)
    images = 0
    for name in os.listdir(args.capture):
        if name.startswith("vu1_") and name.endswith(".bin"):
            if name[4:-4] in EXCLUDED:
                continue
            shutil.copy2(os.path.join(args.capture, name), filtered)
            images += 1
    with open(os.path.join(args.capture, "entries.txt")) as src, open(os.path.join(filtered, "entries.txt"), "w") as dst:
        dst.writelines(line for line in src if line.split()[:1] and line.split()[0] not in EXCLUDED)
    r = subprocess.run([RECOMP, filtered, staging], capture_output=True, text=True)
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
    print(f"{images} images ({len(EXCLUDED)} left out) -> {len(new)} files in generated/vu1: {written} written, {removed} removed")


if __name__ == "__main__":
    main()
