"""Lays out a synthetic PSP memory stick for the sigil cross-check (tests/sigil/check.sh).

Every byte is made up: save folders of the made-up game SKIF00001, each PARAM.SFO built here in the
PSP's layout with made-up values (no real save is involved), plus what a collect must leave out:
a game-data install (a PARAM.SFO with neither save key) and another game's folder. The folder
SKIF00001ORDER holds names whose order differs between sorting names (RomM) and sorting
"name:md5" lines.

Usage: python3 -I tests/sigil/make_tree.py <device dir>
"""

import os
import struct
import sys

GAME = "SKIF00001"


def sfo(fields):
    """A PARAM.SFO: header, index, keys padded to 4 bytes, then each value's reserved bytes."""
    keys = b"".join(key.encode() + b"\0" for key, _, _ in fields)
    key_table = 20 + 16 * len(fields)
    data_table = (key_table + len(keys) + 3) // 4 * 4
    index, data, key_at = b"", b"", 0
    for key, fmt, value in fields:
        reserved = (len(value) + 3) // 4 * 4
        index += struct.pack("<HHIII", key_at, fmt, len(value), reserved, len(data))
        data += value + b"\0" * (reserved - len(value))
        key_at += len(key) + 1
    header = b"\0PSF" + struct.pack("<IIII", 0x0101, key_table, data_table, len(fields))
    return (header + index + keys).ljust(data_table, b"\0") + data


def save_sfo(folder):
    return sfo([
        ("CATEGORY", 0x0204, b"MS\0"),
        ("SAVEDATA_DIRECTORY", 0x0204, folder.encode() + b"\0"),
        ("SAVEDATA_FILE_LIST", 0x0004, bytes(range(256)) * 12 + b"\x5a" * 96),
        ("SAVEDATA_PARAMS", 0x0004, b"\xa5" * 128),
        ("TITLE", 0x0204, b"Skiff cross-check\0"),
    ])


def install_sfo(folder):
    return sfo([
        ("CATEGORY", 0x0204, b"MS\0"),
        ("SAVEDATA_DIRECTORY", 0x0204, folder.encode() + b"\0"),
        ("TITLE", 0x0204, b"Skiff cross-check data\0"),
    ])


def tree():
    data00 = GAME + "DATA00"
    data01 = GAME + "DATA01"
    install = GAME + "INSTALL"
    return {
        f"{data00}/PARAM.SFO": save_sfo(data00),
        f"{data00}/DATA.BIN": bytes((i * 7) & 0xFF for i in range(65536)),
        f"{data00}/ICON0.PNG": b"\x89PNG synthetic icon",
        f"{data01}/PARAM.SFO": save_sfo(data01),
        f"{data01}/DATA.BIN": b"slot one",
        f"{data01}/EMPTY.BIN": b"",
        f"{GAME}NOSFO/DATA.BIN": b"a folder without PARAM.SFO stays with the saves",
        f"{GAME}ORDER/DATA": b"one",
        f"{GAME}ORDER/DATA.BIN": b"two",
        f"{GAME}ORDER/DATA1": b"three",
        f"{install}/PARAM.SFO": install_sfo(install),
        f"{install}/DISC.DAT": b"installed game data, never synced",
        "SKIF00002DATA00/DATA.BIN": b"another game",
    }


def main():
    savedata = os.path.join(sys.argv[1], "PSP", "SAVEDATA")
    for name, content in tree().items():
        path = os.path.join(savedata, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(content)
    print(f"{len(tree())} synthetic files under {savedata}")


if __name__ == "__main__":
    main()
