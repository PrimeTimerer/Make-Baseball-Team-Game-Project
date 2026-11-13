"""Check Korean translations of the executable's strings: py -3 tools/korean/exe_check.py [file.json ...]

Without arguments it checks tools/korean/exe/ko*.json. Each file is {"<address>": "Korean"} for addresses of exe/source.json.
Rules: the Korean bytes fit "max" (a Hangul syllable or a full-width character is 2 bytes, ASCII 1), the same format
specifiers in the same order, the same line breaks, no Japanese kana or kanji left.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FORMAT = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[hlL]?[diouxXeEfgGcsp%]")


def size(text):
    """Bytes in the game: Hangul 2 (a kanji code), the middle dot 2, otherwise Shift-JIS; None if it can't be written."""
    n = 0
    for ch in text:
        if "가" <= ch <= "힣" or ch == "·":
            n += 2
            continue
        try:
            n += len(ch.encode("cp932"))
        except UnicodeEncodeError:
            return None
    return n


def japanese_left(text):
    return [ch for ch in text if "぀" <= ch <= "ヿ" or "一" <= ch <= "鿿" or "ｦ" <= ch <= "ﾟ"]


def check(path, source):
    problems = []
    for addr, ko in json.load(open(path, encoding="utf-8")).items():
        if addr not in source:
            problems.append("%s: no such address in exe/source.json" % addr)
            continue
        ja, mx = source[addr]["ja"], source[addr]["max"]
        n = size(ko)
        if n is None:
            problems.append("%s: a character the game can't show: %r" % (addr, ko))
        elif n > mx and not source[addr].get("pack"):  # packed strings: the build checks the area
            problems.append("%s: %d bytes > %d: %s  (ja: %s)" % (addr, n, mx, ko, ja))
        if FORMAT.findall(ko) != FORMAT.findall(ja):
            problems.append("%s: format %s, the original has %s: %s" % (addr, FORMAT.findall(ko), FORMAT.findall(ja), ko))
        if ko.count("\n") != ja.count("\n"):
            problems.append("%s: %d line breaks, the original has %d" % (addr, ko.count("\n"), ja.count("\n")))
        left = japanese_left(ko)
        if left:
            problems.append("%s: Japanese left (%s): %s" % (addr, "".join(left), ko))
        if not ko.strip():
            problems.append("%s: empty" % addr)
    return problems


def main():
    source = json.load(open(os.path.join(HERE, "exe", "source.json"), encoding="utf-8"))
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(HERE, "exe", "ko*.json")))
    total = 0
    for path in files:
        problems = check(path, source)
        total += len(problems)
        for p in problems:
            print("%s: %s" % (os.path.basename(path), p))
    print("%d problems in %d files" % (total, len(files)))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
