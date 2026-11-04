"""Writes a formatted 8 MB PS2 memory card image (Mcd001.ps2), empty or with saves copied from folders: the layout of
a card formatted by the console (the same as mymc's and PCSX2's): 528-byte raw pages (512 data + 16 spare: three
Hamming ECC bytes per 128-byte chunk, then zeros), superblock, one indirect FAT cluster, 32 FAT clusters, the root
directory ("." and "..") and two reserved backup erase blocks.

--import <folder> adds every subfolder of <folder> as a save directory with its files, laid out as YKTK3's MCMAN
writes them (directory mode 0x842F, files 0x8497, "." pointing at the parent's cluster and its entry there): the
folder cards of other emulators and of the previous YKTK3 build (game/saves/early-play-mc0) become a card the game
reads with "続きから".

Usage: python tools/make_memcard.py <out.ps2> [--import <folder>] [--force]
"""
import argparse
import os
import struct
import time

PAGE = 512
SPARE = 16
PAGES_PER_CLUSTER = 2
PAGES_PER_BLOCK = 16
PAGES = 16384
CLUSTER = PAGE * PAGES_PER_CLUSTER
CLUSTERS = PAGES // PAGES_PER_CLUSTER  # 8192
BLOCKS = PAGES // PAGES_PER_BLOCK  # 1024
CLUSTERS_PER_BLOCK = PAGES_PER_BLOCK // PAGES_PER_CLUSTER  # 8
ENTRIES_PER_CLUSTER = CLUSTER // 4  # 256
IFC = 8  # the indirect FAT cluster
FAT_CLUSTERS = CLUSTERS // ENTRIES_PER_CLUSTER  # 32
ALLOC_OFFSET = IFC + 1 + FAT_CLUSTERS  # 41
GOOD_BLOCKS = BLOCKS - 2  # the last two blocks are the backup blocks
ALLOC_END = GOOD_BLOCKS * CLUSTERS_PER_BLOCK - ALLOC_OFFSET  # 8135
FREE, END, ALLOCATED = 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000
MODE_ROOT_DOT, MODE_ROOT_DOTDOT = 0x8427, 0xA426
MODE_DOT, MODE_SAVE_DIR, MODE_FILE = 0x8427, 0x842F, 0x8497


def parity(b):
    b ^= b >> 4
    b ^= b >> 2
    b ^= b >> 1
    return b & 1


PARITY = [parity(b) for b in range(256)]
COLUMN_MASKS = [sum(PARITY[b & m] << i for i, m in enumerate((0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0))) for b in range(256)]


def ecc128(chunk):
    column, line0, line1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(chunk):
        column ^= COLUMN_MASKS[b]
        if PARITY[b]:
            line0 ^= ~i
            line1 ^= i
    return bytes((column & 0xFF, line0 & 0x7F, line1 & 0xFF))


def raw_page(data):
    ecc = b"".join(ecc128(data[i:i + 128]) for i in range(0, PAGE, 128))
    return data + ecc + b"\0" * (SPARE - len(ecc))


def tod(t):
    # sceMcStDateTime: unused, sec, min, hour, day, month, year (u16), JST like the console's clock
    lt = time.gmtime(t + 9 * 3600)
    return struct.pack("<BBBBBBH", 0, lt.tm_sec, lt.tm_min, lt.tm_hour, lt.tm_mday, lt.tm_mon, lt.tm_year)


def dir_entry(mode, length, cluster, dir_entry_index, name, stamp):
    entry = struct.pack("<HHI", mode, 0, length) + stamp + struct.pack("<II", cluster, dir_entry_index) + stamp
    entry += struct.pack("<I", 0)  # attr
    entry = entry.ljust(0x40, b"\0") + name.ljust(32, b"\0")
    return entry.ljust(PAGE, b"\0")


class Card:
    def __init__(self):
        self.clusters = [bytearray(b"\xff" * CLUSTER) for _ in range(CLUSTERS)]
        self.fat = [FREE] * ALLOC_END + [END] * (FAT_CLUSTERS * ENTRIES_PER_CLUSTER - ALLOC_END)
        self.next_free = 0

    def alloc(self, count):
        """count clusters (relative to ALLOC_OFFSET) chained in the FAT, in order."""
        chain = list(range(self.next_free, self.next_free + count))
        if not chain or chain[-1] >= ALLOC_END:
            raise SystemExit("the saves do not fit on an 8 MB card")
        self.next_free += count
        for a, b in zip(chain, chain[1:]):
            self.fat[a] = ALLOCATED | b
        self.fat[chain[-1]] = END
        return chain

    def write_chain(self, chain, data, fill=b"\0"):
        data = data.ljust(len(chain) * CLUSTER, fill)
        for i, c in enumerate(chain):
            self.clusters[ALLOC_OFFSET + c][:] = data[i * CLUSTER:(i + 1) * CLUSTER]

    def write_entries(self, chain, entries):
        # Unused slots stay erased (0xFF), as MCMAN leaves them; the directory length bounds every read.
        data = b"".join(entries)
        self.write_chain(chain, data, fill=b"\xff")

    def image(self):
        superblock = bytearray(b"\xff" * CLUSTER)
        header = b"Sony PS2 Memory Card Format ".ljust(28, b"\0") + b"1.2.0.0".ljust(12, b"\0")
        header += struct.pack("<HHHH", PAGE, PAGES_PER_CLUSTER, PAGES_PER_BLOCK, 0xFF00)
        header += struct.pack("<IIIIII", CLUSTERS, ALLOC_OFFSET, ALLOC_END, 0, BLOCKS - 1, BLOCKS - 2)
        header = header.ljust(0x50, b"\0")
        header += struct.pack("<32I", IFC, *([0] * 31))  # indirect FAT cluster list
        header += struct.pack("<32i", *([-1] * 32))  # bad block list
        header += bytes((2, 0x52))  # card type (PS2), flags (ECC, bad blocks, erase zeroes as the console writes)
        superblock[:len(header)] = header
        self.clusters[0][:] = superblock
        self.clusters[IFC][:] = struct.pack(f"<{ENTRIES_PER_CLUSTER}I", *([IFC + 1 + i for i in range(FAT_CLUSTERS)] +
                                                                       [0] * (ENTRIES_PER_CLUSTER - FAT_CLUSTERS)))
        for i in range(FAT_CLUSTERS):
            self.clusters[IFC + 1 + i][:] = struct.pack(f"<{ENTRIES_PER_CLUSTER}I",
                                                       *self.fat[i * ENTRIES_PER_CLUSTER:(i + 1) * ENTRIES_PER_CLUSTER])
        out = bytearray()
        for cluster in self.clusters:
            for p in range(PAGES_PER_CLUSTER):
                out += raw_page(bytes(cluster[p * PAGE:(p + 1) * PAGE]))
        return bytes(out)


def build(import_folder=None):
    card = Card()
    saves = []
    if import_folder:
        for name in sorted(os.listdir(import_folder)):
            path = os.path.join(import_folder, name)
            if os.path.isdir(path):
                files = [(f, os.path.join(path, f)) for f in sorted(os.listdir(path)) if os.path.isfile(os.path.join(path, f))]
                saves.append((name, path, files))
    stamp = tod(time.time())
    root_count = 2 + len(saves)
    root_chain = card.alloc((root_count + 1) // 2)  # starts at cluster 0, the superblock's root directory
    root_entries = [dir_entry(MODE_ROOT_DOT, root_count, 0, 0, b".", stamp),
                    dir_entry(MODE_ROOT_DOTDOT, 0, 0, 0, b"..", stamp)]
    for index, (name, path, files) in enumerate(saves, start=2):
        dir_stamp = tod(os.path.getmtime(path))
        count = 2 + len(files)
        dir_chain = card.alloc((count + 1) // 2)
        entries = [dir_entry(MODE_DOT, 0, root_chain[0], index, b".", dir_stamp),
                   dir_entry(MODE_DOT, 0, 0, 0, b"..", dir_stamp)]
        for file_name, file_path in files:
            data = open(file_path, "rb").read()
            chain = card.alloc(max(1, (len(data) + CLUSTER - 1) // CLUSTER)) if data else []
            if chain:
                card.write_chain(chain, data)
            entries.append(dir_entry(MODE_FILE, len(data), chain[0] if chain else 0, 0, file_name.encode("ascii"),
                                     tod(os.path.getmtime(file_path))))
        card.write_entries(dir_chain, entries)
        root_entries.append(dir_entry(MODE_SAVE_DIR, count, dir_chain[0], 0, name.encode("ascii"), dir_stamp))
        print(f"  {name}: {len(files)} files, {sum(os.path.getsize(p) for _, p in files)} bytes")
    card.write_entries(root_chain, root_entries)
    return card.image(), ALLOC_END - card.next_free


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--import", dest="import_folder", help="a folder whose subfolders are save directories")
    ap.add_argument("--force", action="store_true", help="replace an existing card (its saves are lost)")
    args = ap.parse_args()
    if os.path.exists(args.out) and not args.force:
        raise SystemExit(f"{args.out} exists; --force replaces it")
    image, free = build(args.import_folder)
    assert len(image) == PAGES * (PAGE + SPARE)
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"{args.out}: formatted 8 MB card, {free} free clusters")


if __name__ == "__main__":
    main()
