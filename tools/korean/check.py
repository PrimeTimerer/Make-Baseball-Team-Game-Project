"""Checks translations against their Japanese sources.

usage: py -3 tools/korean/check.py NAME [NAME ...]     (NAME = message file without .PRS, e.g. EVENT_MES)
       py -3 tools/korean/check.py --all

For each string of text/NAME.json against source/NAME.json:
  - every source string is translated, no extra keys;
  - the control codes {xxxx} are the same and in the same order (line breaks {010a} may move);
  - no Japanese left (kana, kanji, half-width kana) and every character can be written (Hangul or Shift-JIS);
  - each line fits the widest line the file had (full-width = 2, ASCII = 1, codes = 0);
  - a page (between {01ffff}) has no more lines than the file's longest page.
Exit code 1 when anything is wrong.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
CODE = re.compile(r"\{[0-9a-f]+\}")


def width(text):
    w = 0
    for c in text:
        o = ord(c)
        w += 1 if o < 0x80 or 0xFF61 <= o <= 0xFF9F else 2
    return w


def pages(t):
    """[[line widths], ...] per page; codes take no space."""
    out = []
    for page in t.split("{01ffff}"):
        out.append([width(CODE.sub("", line)) for line in page.split("{010a}")])
    return out


def codes(t):
    return [c for c in CODE.findall(t) if c != "{010a}"]


def japanese(c):
    """Kana, kanji or half-width kana (the middle dot ・ is allowed: Korean uses it too and the font has it)."""
    o = ord(c)
    return (0x3040 <= o <= 0x30FF and o != 0x30FB) or 0x4E00 <= o <= 0x9FFF or 0xFF61 <= o <= 0xFF9F


def check(name):
    src = json.load(open(os.path.join(HERE, "source", name + ".json"), encoding="utf-8"))
    path = os.path.join(HERE, "text", name + ".json")
    if not os.path.exists(path):
        return ["%s: no text/%s.json" % (name, name)], set()
    tr = json.load(open(path, encoding="utf-8"))
    errors = []
    maxw = max((w for t in src.values() for p in pages(t) for w in p), default=0)
    maxl = max((len(p) for t in src.values() for p in pages(t)), default=1)
    syll = set()
    for k in src:
        if k not in tr:
            errors.append("%s[%s]: not translated" % (name, k))
    for k, t in tr.items():
        if k not in src:
            errors.append("%s[%s]: no such source string" % (name, k))
            continue
        if codes(t) != codes(src[k]):
            errors.append("%s[%s]: control codes differ: %s vs source %s" % (name, k, "".join(codes(t)), "".join(codes(src[k]))))
        plain = CODE.sub("", t)
        left = [c for c in plain if japanese(c)]
        if left:
            errors.append("%s[%s]: Japanese left: %s" % (name, k, "".join(left)))
        for c in plain:
            if 0xAC00 <= ord(c) <= 0xD7A3:
                syll.add(c)
                continue
            try:
                c.encode("cp932")
            except UnicodeEncodeError:
                errors.append("%s[%s]: cannot be written in the game: %r" % (name, k, c))
        for i, p in enumerate(pages(t)):
            if len(p) > maxl:
                errors.append("%s[%s]: page %d has %d lines (max %d)" % (name, k, i + 1, len(p), maxl))
            for w in p:
                if w > maxw:
                    errors.append("%s[%s]: a line is %d wide (max %d)" % (name, k, w, maxw))
    # The game keeps some tables in fixed-size buffers (packages in SET/*.FPS): the rebuilt table must not be bigger
    # than the original (Hangul = 2 bytes, ASCII = 1, codes as written).
    if not errors:
        size = table_size(name, src, tr)
        if size and size[1] > size[0]:
            errors.append("%s: table is %d bytes, %d more than the original %d; shorten some strings" %
                          (name, size[1], size[1] - size[0], size[0]))
    return errors, syll


def table_size(name, src, tr):
    """(original, translated) size of the rebuilt TBB1 table, or None when the disc is not available."""
    try:
        import cvm
        import mes
    except ImportError:
        return None
    try:
        d = cvm.prs(cvm.read("/MESSAGE/%s.PRS" % name))
    except Exception:
        return None
    strings = mes.strings(d)
    new = list(strings)
    for k, t in tr.items():
        n = 0
        for part in re.split(r"(\{[0-9a-f]+\})", t):
            if re.fullmatch(r"\{(?:[0-9a-f]{2})+\}", part):
                n += len(part[1:-1]) // 2
            else:
                for c in part:
                    n += 2 if 0xAC00 <= ord(c) <= 0xD7A3 else len(c.encode("cp932"))
        if strings[int(k)].endswith(b"\0"):
            n += 1
        new[int(k)] = b"x" * n
    return len(d), len(mes.rebuild(d, new))


def main():
    names = sys.argv[1:]
    if names == ["--all"]:
        names = sorted(f[:-5] for f in os.listdir(os.path.join(HERE, "text")) if f.endswith(".json") and not f.endswith("_PAC.json"))
    all_errors, all_syll = [], set()
    for n in names:
        e, s = check(n)
        all_errors += e
        all_syll |= s
        print("%s: %s" % (n, "OK" if not e else "%d problems" % len(e)))
    for e in all_errors:
        print("  " + e)
    print("Hangul syllables used: %d" % len(all_syll))
    sys.exit(1 if all_errors else 0)


if __name__ == "__main__":
    main()
