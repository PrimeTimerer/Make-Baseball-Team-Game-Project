"""The YKTK3 release folder and ZIP (python build.py release on Windows): build/release/YKTK3-PC-<version>/ and its .zip.

What goes in (one folder; the launcher finds yktk3.exe next to it):
  YKTK3 Launcher.exe, yktk3.exe and every DLL next to it, PCSX2's GS resources and shader cache,
  cheats_native.txt (the cheats compiled into yktk3.exe), player_cheat_items.txt, the mods folder (.pnach cheats,
  their notes), a config.ini with no disc path and no cheat switched on, the default controls.ini,
  README.txt, LICENSE.txt, THIRD-PARTY.txt.
What never goes in: anything from the disc (game_data, RAM and VU1 dumps), what the game or the launcher writes while
it runs (memcards, states, logs, cheat_saves, cheat_values, player_cheats.ini, scout_cheats.ini, match_script.ini,
staff_cheats.ini, ichinen_cheats.ini, ability_limits.ini, ichinen_skills.bin, launcher.ini, profiles), debug files
(.pdb, .ilk) and backups (.bak*).

usage: py -3 tools/release_yktk3.py [version, default today's date]"""
import datetime
import hashlib
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build" / "port" / "bin"
RELEASE = ROOT / "build" / "release"
REPO = "https://github.com/PrimeTimerer/Make-Baseball-Team-Game-Project"

FILES = ["yktk3.exe", "YKTK3 Launcher.exe", "cheats_native.txt", "player_cheat_items.txt", "controls.ini"]
FOLDERS = ["resources", "shadercache_pcsx2", "mods"]
# Never, wherever they are (checked again on the finished folder).
FORBIDDEN_NAMES = {"memcards", "states", "logs", "game_data", "cheat_saves", "cheat_values", "player_cheats.ini",
                   "scout_cheats.ini", "match_script.ini", "staff_cheats.ini", "ichinen_cheats.ini", "ability_limits.ini",
                   "ichinen_skills.bin", "launcher.ini", "scout_models.tsv", "SLPM_658.53"}
FORBIDDEN_SUFFIXES = {".pdb", ".ilk", ".iso", ".ps2", ".state", ".ram", ".exp", ".lib"}
FORBIDDEN_PATTERNS = [re.compile(p, re.I) for p in (r"\.bak", r"^ram_.*\.bin$", r"^vu1.*\.bin$", r"^perfil_", r"^perf-")]


def forbidden(path: Path) -> bool:
    name = path.name
    return (name in FORBIDDEN_NAMES or path.suffix.lower() in FORBIDDEN_SUFFIXES
            or any(p.search(name) for p in FORBIDDEN_PATTERNS))


def clean_config(text: str) -> str:
    """config.ini without the owner's disc path, cheats or DLL mods switched on and texture folder."""
    out = []
    for line in text.splitlines():
        key = line.split("=", 1)[0].strip()
        if not line.lstrip().startswith("#") and key in ("Iso", "Enabled", "Dlls", "HDTexturesFolder"):
            line = key + "="
        out.append(line)
    return "\n".join(out) + "\n"


README = """프로야구 팀을 만들자! 3 (プロ野球チームをつくろう！3) PC 포트 {version}
=====================================================================
PlayStation 2 판 (SEGA 2005, SLPM-65853) 의 PC 네이티브 포트입니다. 비영리 팬 프로젝트이며 SEGA와 관계없습니다.
소스: {repo}

시작하기
1. "YKTK3 Launcher.exe" 를 엽니다.
2. 직접 가지고 있는 정품 디스크로 만든 이미지(.iso, SLPM-65853)를 고릅니다. 디스크 이미지는 들어 있지 않습니다.
   yktk3.exe 는 실행할 때 디스크의 실행 파일(SLPM_658.53)을 SHA-256 으로 대조해 정품 디스크에서만 동작합니다.
3. 플레이를 누릅니다. 화면·컨트롤러·메모리카드·치트는 런처에서 정합니다.

알아 둘 것
- yktk3.exe 에는 게임의 실행 코드(SEGA)를 C++ 로 변환한 코드가 들어 있습니다. 게임과 그 안의 모든 데이터·코드의 권리는
  SEGA 에 있습니다. 게임 데이터(그래픽·소리 등)는 각자의 디스크 이미지에서 읽습니다.
- 게임 중 단축키: F1 단축키 목록, F2 게임 메뉴(선수 치트·치트·스태프 치트·이치넨·구장·시설·경기 스크립트·99 제한·
  스카우트), F5 빠른 저장, F9 빠른 불러오기, F7 200%, F8 터보, F11 전체 화면.
- F2 메뉴의 설정은 세이브마다 따로 cheat_saves 폴더에 저장되고, 새 게임은 빈 설정으로 시작합니다(새 세이브는 '99 제한
  풀기'·'이치넨 99 상한 풀기'·최종값 상한 교타 200이 켜진 채). 선수 치트는 값을 정한 뒤 '적용'(1회) 또는 '적용 및 고정'을
  눌러야 씁니다. 능력치는 127까지(화면에는 S+), 그 위는 이치넨 스킬 보너스로 올립니다.
- mods 폴더의 .pnach 치트와 player_cheat_items.txt(팀 S+ 항목)는 커뮤니티가 만든 치트를 이 포트에 맞춘 것입니다.
  런처의 치트 페이지에서 켜고 끕니다.
- 필요한 것: Windows 10/11 64비트, DirectX 11 그래픽카드, SSE4.1 CPU.

라이선스: GPL-3.0 (LICENSE.txt). 사용한 라이브러리: THIRD-PARTY.txt. 소스는 위 GitHub 저장소에 있습니다.
"""


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else datetime.date.today().strftime("%Y.%m.%d")
    name = f"YKTK3-PC-{version}"
    folder = RELEASE / name
    for needed in ("yktk3.exe", "YKTK3 Launcher.exe", "pcsx2gs.dll"):
        if not (BIN / needed).exists():
            print(f"{BIN / needed} is missing: run python build.py build first.")
            return 1
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    for file in FILES:
        if (BIN / file).exists():
            shutil.copy2(BIN / file, folder / file)
    for dll in sorted(BIN.glob("*.dll")):
        shutil.copy2(dll, folder / dll.name)
    for sub in FOLDERS:
        source = BIN / sub
        if source.is_dir():
            shutil.copytree(source, folder / sub, ignore=lambda d, names: [n for n in names if forbidden(Path(d) / n)])
    config = BIN / "config.ini"
    if config.exists():
        (folder / "config.ini").write_text(clean_config(config.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")
    (folder / "README.txt").write_text(README.format(version=version, repo=REPO), encoding="utf-8")
    shutil.copy2(ROOT / "LICENSE", folder / "LICENSE.txt")
    third = ROOT / "thirdparty" / "README.md"
    if third.exists():
        shutil.copy2(third, folder / "THIRD-PARTY.txt")
    # Check the finished folder once more: nothing personal, nothing from the disc, no debug files.
    bad = [p for p in folder.rglob("*") if forbidden(p)]
    if bad:
        print("refusing: these must not be in a release:", *bad, sep="\n  ")
        return 1
    archive = RELEASE / f"{name}.zip"
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                z.write(path, Path(name) / path.relative_to(folder))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    files = sum(1 for p in folder.rglob("*") if p.is_file())
    print(f"{archive} ({archive.stat().st_size / 1048576:.1f} MB, {files} files)\nSHA-256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
