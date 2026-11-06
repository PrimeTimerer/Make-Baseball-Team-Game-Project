"""Overseas players' real-life models for the F2 스카우트 tab: reads gamezukushi.com's YKTK3 foreign-player lists (one page
per position, a モデル column) and writes scout_models.tsv (name<TAB>model<TAB>league) next to the game. The list is
that site's work, so it is downloaded on the player's machine and not kept in this repository.

usage: py -3 tools/scout_models.py [output folder, default build/port/bin]"""
import pathlib
import re
import sys
import time
import urllib.request
from html.parser import HTMLParser

PAGES = ["foreign-pitcher", "foreign-catcher", "foreign-first", "foreign-second", "foreign-third", "foreign-shortstop",
         "foreign-outfielder"]
BASE = "https://gamezukushi.com/yakyutsuku3/"


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


def main():
    out_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1] / "build" / "port" / "bin"
    best = {}
    for page in PAGES:
        request = urllib.request.Request(BASE + page, headers={"User-Agent": "Mozilla/5.0 (scout_models.py)"})
        with urllib.request.urlopen(request, timeout=30) as response:
            text = response.read().decode("utf-8", errors="replace")
        parser = Tables()
        parser.feed(text)
        rows = 0
        for table in parser.tables:
            if not table or "名前" not in table[0] or "モデル" not in table[0]:
                continue
            head = table[0]
            name_at, model_at = head.index("名前"), head.index("モデル")
            league_at = head.index("リーグ") if "リーグ" in head else None
            for row in table[1:]:
                if len(row) != len(head):
                    continue
                name, model = row[name_at], row[model_at]
                league = row[league_at] if league_at is not None else ""
                old = best.get(name)
                # A real name wins over 架空 / 現役 / an empty cell (players are listed under several positions).
                if old is None or (not old[0] and model) or (old[0] in ("架空", "現役") and model not in ("", "架空", "現役")):
                    best[name] = (model, league)
                rows += 1
        print(f"{page}: {rows} rows")
        time.sleep(1)
    out = out_dir / "scout_models.tsv"
    with open(out, "w", encoding="utf-8", newline="\n") as file:
        file.write("# YKTK3 overseas players' models, from gamezukushi.com/yakyutsuku3 (made by tools/scout_models.py)\n")
        for name, (model, league) in sorted(best.items()):
            file.write(f"{name}\t{model}\t{league}\n")
    real = sum(1 for model, _ in best.values() if model not in ("", "架空", "現役"))
    print(f"{out}: {len(best)} players, {real} with a real model")


if __name__ == "__main__":
    main()
