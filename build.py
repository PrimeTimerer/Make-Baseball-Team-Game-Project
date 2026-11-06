#!/usr/bin/env python3
"""YKTK3 PC: build and run.

    python build.py              numbered menu
    python build.py build        configure (first time) and compile yktk3.exe (urban on Linux)
    python build.py play [args]  run yktk3.exe (extra arguments go to it)
    python build.py generate     recompile the game code and the VU1 microcode from your disc image in game/
    python build.py configure    run CMake again
    python build.py bench        measure frames/s on a savestate from bench/states
    python build.py release      the release folder and ZIP in build/release (no disc data); on Linux the .tar.gz and
                                 the .deb, built in the Steam Runtime container (docs/LINUX.md)

Needs Python 3.10+ and your own YKTK3 (SLPM-65853) disc image in game/. Windows: Visual Studio 2022 with C++ (cl, CMake
and Ninja come with it; set VSROOT if it is not in the default folder). Linux: Clang, CMake, Ninja and the libraries of
tools/linux/install_deps.sh (docs/LINUX.md).
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"
# Windows: build/port with MSVC; Linux: build/linux with Clang (docs/LINUX.md).
BUILD = ROOT / "build" / ("port" if WINDOWS else "linux")
BIN = BUILD / "bin"
EXE = "yktk3.exe" if WINDOWS else "yktk3"
VSENV = ROOT / "tools" / "win" / "vsenv.cmd"
JOBS = os.environ.get("URBAN_JOBS", "8" if WINDOWS else str(os.cpu_count() or 8))


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], **kwargs).returncode


def in_vs(*cmd):
    """Runs a command inside the Visual Studio x64 environment (tools/win/vsenv.cmd); on Linux, as it is.

    Windows: under the system's OEM code page, put back afterwards. cl writes its /showIncludes notes ("참고: 포함 파일:"
    on a Korean Visual Studio) in the console's code page, and ninja matches them byte for byte against the prefix CMake
    found at configure time. From a UTF-8 console (65001) they did not match, so ninja kept no header dependencies and a
    changed header rebuilt nothing that includes it."""
    if not WINDOWS:
        return run(list(cmd))
    import ctypes
    kernel32 = ctypes.windll.kernel32
    old_out, old_in, oem = kernel32.GetConsoleOutputCP(), kernel32.GetConsoleCP(), kernel32.GetOEMCP()
    switch = old_out not in (0, oem)  # 0: no console
    if switch:
        kernel32.SetConsoleOutputCP(oem)
        kernel32.SetConsoleCP(oem)
    try:
        return run(["cmd", "/c", VSENV, *cmd])
    finally:
        if switch:
            kernel32.SetConsoleOutputCP(old_out)
            kernel32.SetConsoleCP(old_in)


def game_running():
    if not WINDOWS:
        return subprocess.run(["pgrep", "-x", EXE], capture_output=True).returncode == 0
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq yktk3.exe", "/NH"], capture_output=True, text=True).stdout
    return "yktk3.exe" in out.lower()


def has_generated_code():
    return (ROOT / "generated" / "register_functions.cpp").exists()


def configure(extra=()):
    args = ["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
    if not WINDOWS:
        # Clang and LLD (the generated code compiles in minutes with Clang; docs/LINUX.md). URBAN_CC/URBAN_CXX override.
        args += [f"-DCMAKE_C_COMPILER={os.environ.get('URBAN_CC', 'clang')}",
                 f"-DCMAKE_CXX_COMPILER={os.environ.get('URBAN_CXX', 'clang++')}"]
        if shutil.which("ld.lld"):
            args.append("-DCMAKE_LINKER_TYPE=LLD")
    # URBAN_DEPS: a FetchContent _deps folder already downloaded (raylib-src, ...) to reuse instead of downloading.
    deps = os.environ.get("URBAN_DEPS")
    if deps:
        for name in ("raylib", "sdl3", "fmt", "elfio", "toml11", "libdwarf", "rabbitizer"):
            src = Path(deps) / f"{name}-src"
            if src.is_dir():
                args.append(f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={src}")
    return in_vs(*args, *extra)


def build(target="urban"):
    if not has_generated_code():
        print("The recompiled game code is missing: run the 'Generate game code' option first.")
        return 1
    if game_running():
        print(f"{EXE} is running: close the game first (the link would fail).")
        return 1
    if not (BUILD / "CMakeCache.txt").exists() and configure():
        return 1
    result = in_vs("cmake", "--build", BUILD, "--target", target, "--", "-j", JOBS)
    if result == 0 and WINDOWS and target == "urban":
        result = build_launcher()
    return result


def build_launcher():
    """Windows: the launcher (tools/launcher: WPF; settings, controllers, memory card, cheats, logs, shortcuts) next to
    yktk3.exe, built with the C# compiler of the .NET Framework 4 that is part of Windows."""
    framework = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Microsoft.NET" / "Framework64" / "v4.0.30319"
    csc = framework / "csc.exe"
    if not csc.exists():
        print(f"{csc} not found: the launcher needs the .NET Framework 4 (part of Windows).")
        return 1
    source = ROOT / "tools" / "launcher"
    references = ["System.Core.dll", "System.Xaml.dll", "System.Xml.dll", "System.Windows.Forms.dll", "System.Drawing.dll",
                  "System.IO.Compression.dll", "System.IO.Compression.FileSystem.dll", "System.Web.Extensions.dll",
                  "Microsoft.VisualBasic.dll",
                  framework / "WPF" / "PresentationCore.dll", framework / "WPF" / "PresentationFramework.dll",
                  framework / "WPF" / "WindowsBase.dll"]
    BIN.mkdir(parents=True, exist_ok=True)
    icon = source / "icon.ico"  # Explorer, the taskbar and the window: a baseball on the infield, the "3" badge
    return run([csc, "/nologo", "/target:winexe", "/optimize+", "/codepage:65001", f"/out:{BIN / 'YKTK3 Launcher.exe'}",
                *([f"/win32icon:{icon}"] if icon.exists() else []),
                *[f"/r:{reference}" for reference in references], f"/resource:{source / 'Theme.xaml'},Theme.xaml",
                *sorted(source.glob("*.cs"))])


def play(args=()):
    exe = BIN / EXE
    if not exe.exists():
        print(f"{EXE} not built yet: run the 'Build' option first.")
        return 1
    return run([exe, *args], cwd=BIN)


def generate(args=()):
    # The recompiler: the prebuilt tools/ps2_recomp.exe (not in the repository; Windows) or built from runtime/ps2xRecomp.
    built = BUILD / "runtime" / "ps2xRecomp" / ("ps2_recomp.exe" if WINDOWS else "ps2_recomp")
    if not (WINDOWS and (ROOT / "tools" / "ps2_recomp.exe").exists()) and not built.exists():
        print("Building the recompiler (ps2_recomp) first.")
        if configure(["-DURBAN_BUILD_RECOMP=ON"]) or in_vs("cmake", "--build", BUILD, "--target", "ps2_recomp", "--", "-j", JOBS):
            return 1
    return run([sys.executable, ROOT / "tools" / "generate.py", *args]) or generate_vu1()


def generate_vu1():
    """The VU1 microcode compiled ahead of time (3D at full speed): tools/vu1_generate.py rebuilds the images from your ELF
    with recomp/vu1_images.txt."""
    tool = BUILD / "runtime" / "ps2xTest" / ("vu1_recomp.exe" if WINDOWS else "vu1_recomp")
    if not tool.exists():
        print("Building the VU1 recompiler (vu1_recomp) first.")
        if configure(["-DURBAN_BUILD_TOOLS=ON"]) or in_vs("cmake", "--build", BUILD, "--target", "vu1_recomp", "--", "-j", JOBS):
            return 1
    return run([sys.executable, ROOT / "tools" / "vu1_generate.py"])


def pick(title, options):
    """Numbered choice; returns the index or None (0 = back)."""
    print(f"\n{title}")
    for i, text in enumerate(options, 1):
        print(f"  {i}) {text}")
    print("  0) Back")
    answer = input("> ").strip()
    if answer.isdigit() and 1 <= int(answer) <= len(options):
        return int(answer) - 1
    return None


def bench(args=()):
    if args:
        return run([sys.executable, ROOT / "tools" / "autotest" / "bench_state.py", *args])
    states = sorted((ROOT / "bench" / "states").glob("*.state"))
    if not states:
        print("No savestates in bench/states.")
        return 1
    choice = pick("Savestate to measure:", [s.stem for s in states])
    if choice is None:
        return 0
    return run([sys.executable, ROOT / "tools" / "autotest" / "bench_state.py", "--state", states[choice],
                "--measure", "20"])


def launcher(args=()):
    return run([sys.executable, ROOT / "tools" / "release.py", "launcher"])


LAUNCHER_LIB = BUILD.parent / "launcher-linux" / "lib"
LAUNCHER_PACKAGES = ["pywebview==6.2.1", "importlib_resources==6.4.5", "typing_extensions==4.13.2"]  # pure Python; the last
# importlib_resources and typing_extensions that still run on Python 3.8 (Ubuntu 20.04)


def launcher_from_source(args=()):
    """Linux: the launcher from launcher/ in pywebview's GTK window (PyGObject and WebKitGTK from the system; pywebview
    installed into build/launcher-linux/lib the first time)."""
    if not (LAUNCHER_LIB / "webview").is_dir():
        if run([sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "-q", "--target", LAUNCHER_LIB,
                *LAUNCHER_PACKAGES]):
            return 1
    env = dict(os.environ, PYTHONPATH=str(LAUNCHER_LIB))
    return run([sys.executable, ROOT / "launcher" / "urban_launcher.py", *args], env=env)


def release(args=()):
    if not WINDOWS:
        return run([sys.executable, ROOT / "tools" / "linux" / "release.py", *args])
    return run([sys.executable, ROOT / "tools" / "release.py"])


def has_disc_image():
    return any(p.suffix.lower() in (".iso", ".bin", ".img") for p in (ROOT / "game").glob("*"))


def open_game_folder():
    if not BIN.exists():
        print("Not built yet.")
    elif WINDOWS:
        os.startfile(BIN)
    else:
        run(["xdg-open", BIN])


# Building from scratch, in order.
STEPS = [
    ("Generate the game code from your disc image (put the .iso in game/ first)", lambda: generate(),
     has_generated_code),
    (f"Build {EXE} (the first time takes over an hour on Windows, a few minutes on Linux)", lambda: build(),
     lambda: (BIN / EXE).exists()),
    ("Play", lambda: play(), None),
]
# Everything else.
UTILITIES = [
    ("Build and play", lambda: build() or play()),
    ("Configure again (CMake)", lambda: configure()),
    ("Build only the PCSX2 GS (pcsx2gs.dll / libpcsx2gs.so)", lambda: build("pcsx2gs")),
    *([("Build only the launcher (YKTK3 Launcher.exe)", lambda: build_launcher())] if WINDOWS else []),
    ("Benchmark a savestate (bench/states)", lambda: bench()),
    (f"Open the game folder ({BIN.relative_to(ROOT).as_posix()})", open_game_folder),
    ("Make a release: folder + ZIP (build/release)" if WINDOWS else "Make a release: .tar.gz + .deb (build/release)",
     lambda: release()),
]
# Local tools that are not part of the repository (their folder is in .gitignore).
if WINDOWS and (ROOT / "launcher" / "urban_launcher.py").exists():
    UTILITIES += [("Launcher: run it from the source", lambda: run([sys.executable, ROOT / "launcher" / "urban_launcher.py"])),
                  ("Launcher: build Urban Reign.exe", lambda: launcher())]
elif (ROOT / "launcher" / "urban_launcher.py").exists():
    UTILITIES += [("Launcher: run it from the source (GTK window)", lambda: launcher_from_source())]


def menu():
    actions = [action for _, action, _ in STEPS] + [action for _, action in UTILITIES]
    while True:
        print("\n=== YKTK3 PC: build ===")
        if not has_disc_image():
            print("  (no disc image in game/ yet: copy your YKTK3 (SLPM-65853) .iso there)")
        for i, (text, _, done) in enumerate(STEPS, 1):
            mark = "  [done]" if done and done() else ""
            print(f"  Step {i}) {text}{mark}")
        print("\n  Utilities")
        for i, (text, _) in enumerate(UTILITIES, len(STEPS) + 1):
            print(f"  {i}) {text}")
        print("\n  0) Exit")
        answer = input("> ").strip()
        if answer in ("0", "q", ""):
            return 0
        if answer.isdigit() and 1 <= int(answer) <= len(actions):
            result = actions[int(answer) - 1]()
            if isinstance(result, int) and result:
                print(f"(exit code {result})")
        else:
            print("Pick a number from the list.")


COMMANDS = {
    "build": lambda a: build(a[0] if a else "urban"),
    "play": play,
    "run": play,
    "generate": generate,
    "configure": configure,
    "bench": bench,
    "launcher": launcher,
    "release": release,
}


def main():
    if len(sys.argv) < 2:
        return menu()
    command, args = sys.argv[1], sys.argv[2:]
    if command not in COMMANDS:
        print(__doc__)
        return 2
    return COMMANDS[command](args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
