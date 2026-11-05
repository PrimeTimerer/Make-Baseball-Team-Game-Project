"""Cheats by function: splits the .pnach files of the mods folder into one file per cheat function, so each can be
turned on and off on its own (F2 > 치트, or the launcher), named from the code table workbook (.xlsx) in the mods
folder when there is one.

A cheat file often holds many cheats under comment headers ("// ticket 65000"), and the code table's entries are
written for a pad: a condition line (E00nFFFD 01B8E102: "while L3 is held") then the ON words, another condition
then the OFF words (the game's own instructions). PCSX2 1.6.0 ignores the condition lines of a "word" .pnach, so
both run every frame and the last one wins. Here every function gets a file of its own with only its ON words:
- the lines of a header block go to the code table entry that writes the same place (an entry's code words, or its
  routine's words at the same address with the same value); lines no entry writes stay with the header's block;
- a write of the game's original instruction to game code (an OFF line) is left out, so the cheat is on whenever
  its file is enabled and off (original code, restored by the .pnach loader) when it is not;
- a function that writes cheat code below the ELF (0xF0000-0xFFFFF) or jumps there gets the register save/restore
  routine of the table's ★必須コード★ (the table: run the codes that need it together with it);
- files of extended (CodeBreaker) lines with conditions or pointers (team S+, money) stay whole.
Identical functions of several files become one file. Writes mods/기능별/*.pnach and mods/기능별/목록.tsv
(file, category, Korean name, Japanese name, source, note) for the game and the launcher. The original files are not
changed. tools/cheat_code.py compiles the code of mods/기능별 too.

usage: python tools/cheat_groups.py [--mods build/port/bin/mods] [--elf build/SLPM_658.53] [--excel <workbook>]
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cheat_code  # noqa: E402  (Elf, ram_address)

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
TEXT = cheat_code.TEXT
CAVE = (0x0F0000, 0x100000)
ROUTINE = (0x0F0FA0, 0x0F1000)  # ★必須コード★: saves/restores registers for the hooks
OUT_DIR = "기능별"
CODE_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})$")
PATCH_LINE = re.compile(r"^\s*patch\s*=\s*(\d+)\s*,\s*EE\s*,\s*([0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*([0-9A-Fa-f]+)", re.I)

# Korean names of the code table's entries (Japanese as in the table)
KOREAN = {
    "★　必須コード　★": "★ 필수 루틴 (레지스터 저장/복원)",
    "チケット代設定": "티켓 가격 설정",
    "外国人枠撤廃": "외국인 선수 제한 철폐",
    "スタッフスキル設定": "스태프 스킬 설정",
    "のびのび野球": "노비노비 야구 (감독 스킬)",
    "キャンプでの新球種 S+ 習得成功": "캠프 신구종 S+ 습득 성공",
    "キャンプでのスイッチ転向成功": "캠프 스위치 타자 전향 성공",
    "キャンプでのコンバート成功": "캠프 포지션 컨버트 성공",
    "翌週に能力爆発イベント起こる&全員能力爆発": "다음 주 능력 폭발 이벤트 + 전원 능력 폭발",
    "成長爆発経験値最大": "성장 폭발 경험치 최대",
    "衰えなし（常に成長？）": "쇠퇴 없음 (항상 성장)",
    "怪我しない": "부상 없음",
    "引退希望出さない": "은퇴 희망 안 냄",
    "契約更改 条件なんでもOK": "계약 갱신 조건 무엇이든 OK",
    "トレード成功（再申請になっても）": "트레이드 성공 (재신청도)",
    "トレード返答1ターン（再申請も）": "트레이드 답변 1턴 (재신청도)",
    "トレード全選手選択可能": "트레이드 전 선수 선택 가능",
    "国内新人交渉1発": "국내 신인 협상 한 번에",
    "外国人交渉1発": "외국인 협상 한 번에",
    "FA交渉1発": "FA 협상 한 번에",
    "練習施設　建設1ターン": "연습 시설 건설 1턴",
    "球場施設　建設1ターン": "구장 시설 건설 1턴",
    "経営企画 効果最大(効果がある項目)": "경영기획 효과 최대",
    "出張で指定アイデアメモGET": "출장에서 지정 아이디어 메모 획득",
    "経営企画　指定企画実行&費用0": "경영기획 지정 기획 실행 + 비용 0",
    "経営企画 指定企画から順番に毎週実行": "경영기획 지정 기획부터 매주 순서대로 실행",
    "指定経営企画無限ループ": "지정 경영기획 무한 루프",
}
# The headers of the cheat files (English as the files have them) when no table entry matches
HEADERS = {
    "master code": "마스터 코드", "club status cheats": "구단 상태",
    "skill and team color spread in labratory": "연구소 스킬·팀 컬러 전파", "scouter max": "스카우터 최대",
    "rookie stat all discover": "신인 능력 전부 공개", "yuhakji jungae": "유학지 전개", "ichinen 99": "ichinen 99",
    "ichinen knuckler": "ichinen 너클러", "ticket 65000": "티켓 가격 최대", "foreigner season": "외국인 제한 철폐",
    "stat explosive": "능력 폭발", "stat exp": "성장 폭발 경험치 최대", "camp succes": "캠프 성공",
    "sisul fast 1turn": "시설 건설 1턴", "contracts": "계약 조건 OK", "stat no decrease": "노쇠 없음",
    "management max": "경영기획 효과 최대", "coaches skills": "코치 스킬 설정",
    "team management even loop": "경영기획 루프", "資金最大その１（par)": "자금 최대 (포인터 코드)",
    "資金最大その１（par）": "자금 최대 (포인터 코드)",
}
# Headers of blocks that hold several cheats: their lines no table entry has are split by game function
MULTI = {"club status cheats", "master code"}
SMALL_FILE = 200  # files of extended lines up to this many lines are split like the others (team S+ stay whole)
CATEGORIES = [  # (category, keywords in the Korean/Japanese/English names), first match
    ("필수", ("필수", "必須")),
    ("자금·구단", ("자금", "資金", "구단 상태", "club status", "티켓", "チケット")),
    ("경영기획", ("경영", "経営", "management")),
    ("시설", ("시설", "施設", "sisul")),
    ("계약·트레이드", ("계약", "契約", "트레이드", "トレード", "협상", "交渉", "외국인", "外国人")),
    ("육성·능력", ("능력", "能力", "성장", "成長", "쇠퇴", "衰え", "노쇠", "캠프", "キャンプ", "부상", "怪我", "은퇴",
                 "引退", "ichinen", "stat")),
    ("스태프·스카우트", ("스킬", "スキル", "코치", "コーチ", "스카우", "スカウ", "신인", "新人", "연구소", "유학")),
    ("팀 S+", ("팀 s+",)),
]


def category_of(*names):
    text = " ".join(n.lower() for n in names if n)
    for category, words in CATEGORIES:
        if any(w.lower() in text for w in words):
            return category
    return "기타"


def excel_entries(path):
    """The code table's entries: [{jp, writes: [(address, value, on)], conditions}] from the コードベース sheet."""
    try:
        import openpyxl
    except ImportError:
        print("cheat groups: openpyxl is not installed (pip install openpyxl): names come from the cheat files only")
        return []
    book = openpyxl.load_workbook(path, read_only=True, data_only=True)
    if "コードベース" not in book.sheetnames:
        return []
    entries, entry, label = [], None, ""
    for row in book["コードベース"].iter_rows(values_only=True):
        cells = [("" if c is None else str(c).strip()) for c in row] + [""] * 4
        start = cells[0].isdigit() and cells[1]
        code = CODE_LINE.match(cells[2])
        if start:
            entry = {"jp": cells[1], "lines": []}
            entries.append(entry)
            label = ""
        elif cells[1]:
            label = cells[1]
        if entry is not None and code:
            entry["lines"].append((int(code[1], 16), int(code[2], 16), label))
    return [e for e in entries if e["lines"]]


def lines_of(path):
    """A .pnach file as blocks: [(header, [(place, address, kind, value, text)])] in file order. The header of a
    block is the first line of the comments above it (the cheat's name; later comment lines explain it)."""
    blocks, header, current, in_comments = [], "", [], False
    for raw in open(path, encoding="utf-8-sig", errors="replace"):
        text = raw.rstrip("\r\n")
        match = PATCH_LINE.match(text)
        stripped = text.strip()
        if match:
            current.append((int(match[1]), int(match[2], 16), match[3].lower(), int(match[4], 16), stripped))
            in_comments = False
        elif stripped.startswith("//") and not PATCH_LINE.match(stripped[2:]):
            if current:
                blocks.append((header, current))
                current = []
            words = stripped.lstrip("/").strip().strip("[]").strip()
            if words and not in_comments:
                header = words
            in_comments = True
        else:
            in_comments = False
    if current:
        blocks.append((header, current))
    return blocks


CIRCLED = "①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳"
# 資金最大 routine of the "Club Status Cheats": a jump at a function's return (0x104D68) to code written over .rodata
# text at 0x68FFC8 that stores 99999990 at a fixed address (PCSX2's 0x866390/0x8664D0). The club object is allocated,
# so here that address is other data (it broke 4 bytes of a save). Same hook, the money of the club object
# [0x62F190] + 0x124 (as tools/cheat_code.py's pointer code), in free memory below the ELF.
MONEY_HOOK = (0x104D68, 0x081A3FF2)
MONEY_CAVE = (0x68FFC8, 0x68FFE0)
MONEY_FIX = [(0x104D68, 0x0803E080),  # j 0xF8200
             (0x0F8200, 0x3C040063), (0x0F8204, 0x8C84F190),  # a0 = [0x62F190]
             (0x0F8208, 0x10800003), (0x0F820C, 0x3C0105F5),  # if a0 == 0 return; at = 99999990
             (0x0F8210, 0x3421E0F6), (0x0F8214, 0xAC810124),  # [a0 + 0x124] = at
             (0x0F8218, 0x03E00008), (0x0F821C, 0x00000000)]  # jr ra


def split(mods, elf_path, functions_path, excel=None):
    """Writes mods/기능별 (one .pnach per cheat function and 목록.tsv); returns the list's rows."""
    elf = cheat_code.Elf(elf_path)

    def original(address):
        return int.from_bytes(bytes(elf.byte(address + i) for i in range(4)), "little")

    bounds = []
    for line in open(functions_path, encoding="utf-8"):
        parts = line.strip().split(",")
        if len(parts) >= 3 and parts[1].startswith("0x"):
            bounds.append((int(parts[1], 16), int(parts[2], 16)))

    def function_of(address):
        return next((start for start, end in bounds if start <= address < end), address & ~0xFFF)

    workbook = excel or next(iter(sorted(glob.glob(os.path.join(mods, "*.xlsx")))), None)
    entries = excel_entries(workbook) if workbook else []
    # Where each entry writes: (address, value); game code also by address for its ON words (a value of the same
    # instruction, e.g. another ticket price, is the same cheat)
    by_word, by_code_on, routine_lines = {}, {}, []
    for index, entry in enumerate(entries):
        on_words = []
        for address, value, label in entry["lines"]:
            if address >> 28 not in (0, 1, 2):
                continue
            physical = address & 0x01FFFFFF
            off = "OFF" in label.upper() or (TEXT[0] <= physical < TEXT[1] and value == original(physical))
            if entry["jp"].startswith("★") or ROUTINE[0] <= physical < ROUTINE[1]:
                if ROUTINE[0] <= physical < ROUTINE[1] and (physical, value) not in routine_lines:
                    routine_lines.append((physical, value))
                continue
            by_word.setdefault((physical, value), set()).add(index)
            if not off:
                on_words.append((physical, value))
                if TEXT[0] <= physical < TEXT[1]:
                    by_code_on.setdefault(physical, []).append((index, value))
        entry["on"] = on_words

    def same_code(a, b):
        """The same instruction, or the same always-taken branch written two ways (beq x,x / blez $0 / bgez $0)."""
        def always(v):
            op, rs, rt = v >> 26, (v >> 21) & 31, (v >> 16) & 31
            return ("b", v & 0xFFFF) if (op == 4 and rs == rt) or (op == 6 and rs == 0) or (op == 1 and rs == 0 and rt == 1) else v
        return always(a) == always(b)

    def owners_of(physical, value):
        owners = by_word.get((physical, value))
        if owners:
            return owners
        exact = {i for i, v in by_code_on.get(physical, ()) if same_code(v, value)}
        return exact or {i for i, v in by_code_on.get(physical, ()) if v >> 26 == value >> 26}

    def needs_routine(lines):
        """Code of this function that calls the register save/restore routine (jal 0xF0FA0 / 0xF0FCC)."""
        return any((v >> 26) in (2, 3) and ROUTINE[0] <= ((v & 0x03FFFFFF) << 2) < ROUTINE[1] for _, v in lines)

    def clusters(lines):
        """Lines no table entry has, by game function; code written outside the game code goes with the line that
        jumps to it."""
        jumps = sorted({(v & 0x03FFFFFF) << 2 for a, v in lines if TEXT[0] <= a < TEXT[1] and v >> 26 in (2, 3)
                        and not TEXT[0] <= (v & 0x03FFFFFF) << 2 < TEXT[1]})
        caller = {(v & 0x03FFFFFF) << 2: function_of(a) for a, v in lines if TEXT[0] <= a < TEXT[1] and v >> 26 in (2, 3)}
        result, order = {}, []
        for a, v in lines:
            if TEXT[0] <= a < TEXT[1]:
                key = function_of(a)
            else:
                targets = [t for t in jumps if t <= a < t + 0x1000]
                key = caller[targets[-1]] if targets else ("data", a & ~0xFFF)
            if key not in result:
                result[key] = []
                order.append(key)
            result[key].append((a, v))
        return [(key, result[key]) for key in order]

    out_dir = os.path.join(mods, OUT_DIR)
    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "*.pnach")):
        os.remove(old)
    groups = {}  # signature -> group
    whole = []   # files kept whole: (file, name)

    def add_group(name, jp, header, lines, source, verbatim=None, note="", entry=None):
        """A function: the same table entry from several files is one (their lines joined, a place once), else the
        same lines are one."""
        signature = ("entry", entry) if entry is not None else tuple(lines) if verbatim is None else tuple(verbatim)
        group = groups.setdefault(signature, {"name": name, "jp": jp, "header": header, "lines": [],
                                              "verbatim": verbatim, "sources": [], "note": note})
        if verbatim is None:
            for address, value in lines:
                if not any(a == address for a, _ in group["lines"]):
                    group["lines"].append((address, value))
        if source not in group["sources"]:
            group["sources"].append(source)

    for name in sorted(f for f in os.listdir(mods) if f.lower().endswith(".pnach")):
        blocks = lines_of(os.path.join(mods, name))
        every = [line for _, block in blocks for line in block]
        if len(every) > SMALL_FILE and all(kind == "extended" for _, _, kind, _, _ in every):
            whole.append((name, os.path.splitext(name)[0]))
            continue
        for header, block in blocks:
            source = f"{name}" + (f" \"{header}\"" if header else "")
            title = HEADERS.get(header.lower(), header or os.path.splitext(name)[0])
            if any(kind == "extended" and address >> 28 not in (0, 1, 2) for _, address, kind, _, _ in block):
                # conditions or pointer codes: the block as it is
                add_group(title, "", header, [], source, verbatim=[text for *_, text in block],
                          note="조건·포인터 코드 그대로")
                continue
            parts, order, previous, skip_next = {}, [], None, False
            for place, address, kind, value, text in block:
                if skip_next:  # the second line of a serial code: data, not a write
                    skip_next = False
                    continue
                if kind != "extended" and address >> 28 == 4:
                    skip_next = True
                physical = cheat_code.ram_address(address) if kind != "extended" else address & 0x01FFFFFF
                if physical is None or (kind == "extended" and address >> 28 not in (0, 1, 2)):
                    continue  # condition, serial and other lines a word .pnach ignores (PCSX2 1.6.0)
                if ROUTINE[0] <= physical < ROUTINE[1]:
                    continue  # the routine goes with the functions that need it
                value &= 0xFFFFFFFF
                if TEXT[0] <= physical < TEXT[1] and value == original(physical):
                    continue  # an OFF line: the game's own instruction
                owners = owners_of(physical, value)
                key = (previous if previous in owners else min(owners)) if owners else (
                    previous if isinstance(previous, int) and not TEXT[0] <= physical < TEXT[1] else ("header",))
                previous = key
                if key not in parts:
                    parts[key] = []
                    order.append(key)
                if (physical, value) not in parts[key]:
                    parts[key].append((physical, value))
            for key in order:
                lines = parts[key]
                if isinstance(key, int):
                    jp = entries[key]["jp"]
                    add_group(KOREAN.get(jp, jp), jp, header, lines, source, entry=key)
                    continue
                pieces = clusters(lines) if header.lower() in MULTI or not header else [(None, lines)]
                for number, (where, piece) in enumerate(pieces):
                    note = ""
                    if MONEY_HOOK in piece:
                        piece = [w for w in piece if not MONEY_CAVE[0] <= w[0] < MONEY_CAVE[1] and w != MONEY_HOOK]
                        piece = MONEY_FIX + piece
                        label = "자금 최대 (구단 상태 루틴)"
                        note = "PCSX2 고정 주소 대신 구단 [0x62F190]+0x124 에 씀"
                    elif len(pieces) == 1:
                        label = title
                    else:
                        start = where if isinstance(where, int) else where[1]
                        label = f"{title} {CIRCLED[number] if number < len(CIRCLED) else number + 1} (0x{start:06X})"
                        note = "기능 이름 미확인: 원본 구획을 게임 함수별로 나눔"
                    add_group(label, "", header, piece, source, note=note)

    # Code table entries no cheat file has (off unless turned on)
    covered = {g["jp"] for g in groups.values() if g["jp"]}
    extra = 0
    for entry in entries:
        if entry["jp"] in covered or entry["jp"].startswith("★") or not entry.get("on"):
            continue
        add_group(KOREAN.get(entry["jp"], entry["jp"]), entry["jp"], "", entry["on"], f"{os.path.basename(workbook)}",
                  note="코드표에만 있음")
        extra += 1
    # One file per function, numbered by category
    rows, used = [], set()
    category_order = [c for c, _ in CATEGORIES] + ["기타"]
    ordered = sorted(groups.values(), key=lambda g: category_order.index(category_of(g["name"], g["jp"], g["header"])))
    for number, group in enumerate(ordered, 1):
        category = category_of(group["name"], group["jp"], group["header"])
        base = re.sub(r'[\\/:*?"<>|]', "_", group["name"])[:60].strip()
        file_name = f"{number:02d} {base}.pnach"
        used.add(file_name)
        routine = group["verbatim"] is None and needs_routine(group["lines"]) and routine_lines
        with open(os.path.join(out_dir, file_name), "w", encoding="utf-8", newline="\n") as out:
            out.write(f"// 기능별 치트 (tools/cheat_groups.py가 원본 치트 파일에서 나눈 것): {group['name']}\n")
            if group["jp"]:
                out.write(f"// 코드표: {group['jp']}\n")
            if group["note"]:
                out.write(f"// 메모: {group['note']}\n")
            out.write("// 원본: " + "; ".join(group["sources"]) + "\n")
            out.write("gametitle=Yakyutsuku3 (jap)\n\n")
            if routine:
                out.write("// ★必須コード★ 레지스터 저장/복원 루틴 (이 치트의 코드가 씀)\n")
                for address, value in routine_lines:
                    out.write(f"patch=1,EE,2{address:07X},word,{value:08X}\n")
                out.write("\n")
            out.write(f"// {group['name']}\n")
            if group["verbatim"] is not None:
                out.write("\n".join(group["verbatim"]) + "\n")
            else:
                for address, value in group["lines"]:
                    out.write(f"patch=1,EE,2{address:07X},word,{value:08X}\n")
        count = len(group["verbatim"]) if group["verbatim"] is not None else len(group["lines"])
        note = f"{count}줄" + (", 필수 루틴 포함" if routine else "") + (f", {group['note']}" if group["note"] else "")
        rows.append((f"{OUT_DIR}/{file_name}", category, group["name"], group["jp"], "; ".join(group["sources"]), note))
    for file_name, title in whole:
        rows.append((file_name, category_of(title), title, "", file_name, "파일 통째 (조건·포인터 코드)"))
    with open(os.path.join(out_dir, "목록.tsv"), "w", encoding="utf-8", newline="\n") as out:
        out.write("# 파일\t분류\t이름\t코드표 이름\t원본\t메모 (tools/cheat_groups.py)\n")
        for row in rows:
            out.write("\t".join(row) + "\n")
    print(f"cheat groups: {len(groups)} functions ({extra} only in the code table) + {len(whole)} files kept whole -> "
          f"{out_dir}" + (f"; code table: {len(entries)} entries" if workbook else ""))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods", default=os.path.join(ROOT, "build", "port" if os.name == "nt" else "linux", "bin", "mods"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "SLPM_658.53"))
    ap.add_argument("--functions", default=os.path.join(ROOT, "recomp", "yktk3_functions.csv"))
    ap.add_argument("--excel", default=None, help="the code table (default: the .xlsx in the mods folder)")
    ap.add_argument("--switch-config", metavar="CONFIG_INI",
                    help="in config.ini [Mods] Enabled, replace each whole cheat file by its functions (a copy is kept)")
    args = ap.parse_args()
    rows = split(args.mods, args.elf, args.functions, args.excel)
    if args.switch_config:
        switch_config(args.switch_config, rows)


def switch_config(path, rows):
    """config.ini [Mods] Enabled: each enabled whole file that has functions in mods/기능별 becomes those functions
    (the same cheats stay on, now one by one). Written in the file's own encoding; config.ini.bak-cheats keeps the old."""
    raw = open(path, "rb").read()
    try:
        text, encoding = raw.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        text, encoding = raw.decode("mbcs" if os.name == "nt" else "cp949"), "mbcs" if os.name == "nt" else "cp949"
    lines = text.split("\n")
    section, changed = "", False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("["):
            section = stripped.strip("[]").strip().lower()
            continue
        if section != "mods" or not stripped.lower().startswith("enabled") or "=" not in stripped:
            continue
        key, value = line.split("=", 1)
        names = [n.strip() for n in value.strip().split(";") if n.strip()]
        result = []
        for name in names:
            functions = [r[0] for r in rows if r[0].startswith(OUT_DIR + "/") and
                         any(s == name or s.startswith(name + " \"") for s in r[4].split("; "))]
            for item in functions or [name]:
                if item not in result:
                    result.append(item)
        if result != names:
            lines[i] = key.rstrip() + "=" + ";".join(result) + ("\r" if line.endswith("\r") else "")
            changed = True
            print(f"config: {len(names)} enabled entries -> {len(result)} (" +
                  ", ".join(n for n in names if n not in result) + " -> functions)")
        break
    if changed:
        open(path + ".bak-cheats", "wb").write(raw)
        open(path, "wb").write("\n".join(lines).encode(encoding))


if __name__ == "__main__":
    main()
