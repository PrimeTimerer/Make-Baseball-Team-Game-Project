"""Japanese sources for translation: source/<FILE>.json = {index: text with control codes as {hex}} (strings with
Japanese only). usage: py -3 tools/korean/extract.py"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cvm  # noqa: E402
import mes  # noqa: E402


def template(s):
    out = []
    for kind, b in mes.tokens(s):
        if kind == "t":
            out.append(b.decode("cp932"))
        else:
            out.append("{" + b.hex() + "}")
    return "".join(out)


def japanese(t):
    return any(0x3040 <= ord(c) <= 0x30FF or 0x4E00 <= ord(c) <= 0x9FFF or 0xFF61 <= ord(c) <= 0xFF9F for c in t)


def main():
    files = cvm.listing()
    os.makedirs(os.path.join(HERE, "source"), exist_ok=True)
    for p in sorted(x for x in files if x.startswith("/MESSAGE/") and x.endswith("_MES.PRS")):
        d = cvm.prs(cvm.read(p, files))
        if d[:4] != b"TBB1":
            continue
        src = {str(k): template(s) for k, s in enumerate(mes.strings(d))}
        src = {k: t for k, t in src.items() if japanese(t)}
        name = p.split("/")[-1][:-4]
        json.dump(src, open(os.path.join(HERE, "source", name + ".json"), "w", encoding="utf-8"), ensure_ascii=False, indent=0)


if __name__ == "__main__":
    main()
