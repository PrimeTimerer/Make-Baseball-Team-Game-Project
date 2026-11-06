"""Player notes for the F2 스카우트 tab: the real-life model of each made-up overseas player, the position and a line of
the main ability limits. Writes scout_models.tsv (id<TAB>name<TAB>model<TAB>position<TAB>summary<TAB>model in English<TAB>model in Korean) next to the
game; the English and Korean names come from tools/scout_model_names.tsv (this repository's own list).

Sources (both are other people's work, so they are read on the player's machine and not kept in this repository):
- a player data workbook (--excel, e.g. やきゅつく3選手データ.xlsx: sheets 투수 / 야수 with ID "lo_hi" hex bytes, 이름, 포지션,
  [게임]모델 and the ability limits), by player id;
- gamezukushi.com's foreign-player lists (one page per position, a モデル column), by name, for models the workbook lacks
  (skip with --no-web).

usage: py -3 tools/scout_models.py [--excel <workbook>] [--no-web] [output folder, default build/port/bin]"""
import argparse
import pathlib
import re
import time
import urllib.request
from html.parser import HTMLParser

PAGES = ["foreign-pitcher", "foreign-catcher", "foreign-first", "foreign-second", "foreign-third", "foreign-shortstop",
         "foreign-outfielder"]
BASE = "https://gamezukushi.com/yakyutsuku3/"
NO_MODEL = ("", "架空", "現役", "-", "None")
POSITIONS = {"先発完投": "선발완투", "先発": "선발", "中継抑え": "중계·마무리", "中継ぎ": "중계", "抑え": "마무리", "捕手": "포수",
             "一塁手": "1루수", "二塁手": "2루수", "三塁手": "3루수", "遊撃手": "유격수", "外野手": "외야수"}
GRADES = [(90, "S+"), (80, "S"), (70, "A+"), (60, "A"), (55, "B+"), (50, "B"), (45, "C+"), (40, "C"), (35, "D+"), (0, "D")]


def grade(value):
    try:
        value = float(value)
    except (TypeError, ValueError):
        return "?"
    return next(name for low, name in GRADES if value >= low)


class Tables(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables, self.row, self.cell = [], None, None

    def handle_starttag(self, tag, attrs):
        if tag == "table":
            self.tables.append([])
        elif tag == "tr" and self.tables:
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None and self.row is not None:
            self.row.append(re.sub(r"\s+", " ", "".join(self.cell)).strip())
            self.cell = None
        elif tag == "tr" and self.row is not None and self.tables:
            self.tables[-1].append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)


def clean(name):
    """The game's name text: full-width spaces and runs of spaces as one space."""
    return " ".join(str(name or "").replace("　", " ").split())


def model_names():
    """tools/scout_model_names.tsv: model text -> (English, Korean)."""
    names = {}
    path = pathlib.Path(__file__).with_name("scout_model_names.tsv")
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) >= 3:
            names[fields[0]] = (fields[1], fields[2])
    return names


def base_model(model):
    """The model text without a trailing ？ / ? or (…) note."""
    model = re.sub(r"[(（].*$", "", model).strip()
    return model.rstrip("？?").strip()


def from_web():
    models = {}
    for page in PAGES:
        request = urllib.request.Request(BASE + page, headers={"User-Agent": "Mozilla/5.0 (scout_models.py)"})
        with urllib.request.urlopen(request, timeout=30) as response:
            text = response.read().decode("utf-8", errors="replace")
        parser = Tables()
        parser.feed(text)
        for table in parser.tables:
            if not table or "名前" not in table[0] or "モデル" not in table[0]:
                continue
            name_at, model_at = table[0].index("名前"), table[0].index("モデル")
            for row in table[1:]:
                if len(row) == len(table[0]) and row[model_at] not in NO_MODEL:
                    models.setdefault(clean(row[name_at]), row[model_at])
        print(f"web {page}: {len(models)} models so far")
        time.sleep(1)
    return models


def from_workbook(path):
    import openpyxl
    book = openpyxl.load_workbook(path, read_only=True, data_only=True)
    players = {}
    for sheet in book.worksheets:
        rows = sheet.iter_rows(values_only=True)
        head = list(next(rows, []) or [])
        if "ID" not in head or "이름" not in head:
            continue
        at = {h: i for i, h in enumerate(head) if h}
        pitcher = "구위 상한값" in at

        def cell(row, key):
            return row[at[key]] if key in at and at[key] < len(row) else None

        for row in rows:
            if not row or not row[0] or "_" not in str(row[0]):
                continue
            low, high = str(row[0]).split("_")[:2]
            pid = int(high, 16) << 8 | int(low, 16)
            position = str(cell(row, "포지션") or "")
            if pitcher:
                speed = str(cell(row, "구속 적성") or "")
                km = speed.split("/")[1] if "/" in speed else "?"
                summary = (f"구속 {km}km · 구위 {grade(cell(row, '구위 상한값'))} · 제구 {grade(cell(row, '제구 상한값'))} · "
                           f"체력 {grade(cell(row, '체력 상한값'))}")
            else:
                contact = max(float(cell(row, "우교타 상한값") or 0), float(cell(row, "좌교타 상한값") or 0))
                batting = cell(row, "타격")
                summary = (f"교타 {grade(contact)} · 장타 {grade(cell(row, '장타 상한값'))} · 주력 {grade(cell(row, '주력 상한값'))}"
                           + (f" · 타격 {float(batting):.0f}" if isinstance(batting, (int, float)) else ""))
            model = str(cell(row, "[게임]모델") or "")
            players.setdefault(pid, (clean(cell(row, "이름")), "" if model in NO_MODEL else model,
                                     POSITIONS.get(position, position), summary))
    print(f"workbook: {len(players)} players")
    return players


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--excel", default=None)
    parser.add_argument("--no-web", action="store_true")
    parser.add_argument("out", nargs="?", default=str(pathlib.Path(__file__).resolve().parents[1] / "build" / "port" / "bin"))
    args = parser.parse_args()
    players = from_workbook(args.excel) if args.excel else {}
    web = {} if args.no_web else from_web()
    named = {name for name, model, _, _ in players.values() if model}
    names = model_names()

    def translated(model):
        english, korean = names.get(model, names.get(base_model(model), ("", "")))
        return f"{english}\t{korean}"

    out = pathlib.Path(args.out) / "scout_models.tsv"
    with open(out, "w", encoding="utf-8", newline="\n") as file:
        file.write("# YKTK3 player notes for the F2 스카우트 tab (tools/scout_models.py): id, name, model, position, summary, model in English, model in Korean\n")
        for pid, (name, model, position, summary) in sorted(players.items()):
            model = model or web.get(name, "")
            file.write(f"{pid}\t{name}\t{model}\t{position}\t{summary}\t{translated(model)}\n")
        for name, model in sorted(web.items()):
            if name not in named and not any(p[0] == name for p in players.values()):
                file.write(f"0\t{name}\t{model}\t\t\t{translated(model)}\n")
    models = sum(1 for line in open(out, encoding="utf-8") if not line.startswith("#") and line.split("\t")[2])
    print(f"{out}: {len(players)} players from the workbook, {models} with a real model")


if __name__ == "__main__":
    main()
