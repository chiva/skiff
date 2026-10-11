"""Writes the synthetic save units tests/unit/test_saves.c reads, and prints their content hashes.

Every byte is made up here: two save folders of the made-up game SKIF00001, each with a stand-in
PARAM.SFO (it is not a real one, so Skiff's collect keeps the folder) and a DATA.BIN. No real save is
involved. The same content as the live probe against RomM 5.4.0, whose server hashed it to
f0c9413915f1a6c3df66129ee6bc744b.

  probe-stored.zip       argosy-sigil's PSP unit, byte for byte (its src/zip_mem.c): what Skiff's
                         writer must produce for these folders
  probe-deflated.zip     as Argosy and grout upload: deflated, with folder entries, in reverse order
  probe-descriptors.zip  deflated and streamed, so each entry has a data descriptor (as Go's zip
                         writer and Python's on a pipe write them)

The deflated files depend on the zlib Python uses; their content and hash do not.

Usage: python3 -I tests/fixtures/saves/make_units.py tests/fixtures/saves
"""

import hashlib
import io
import struct
import sys
import zipfile
import zlib

GAME = "SKIF00001"
EXPECTED_HASH = "f0c9413915f1a6c3df66129ee6bc744b"
DOS_DATE_1980_01_01 = 0x0021
VERSION_20 = 20


def sfo():
    return b"\x00PSF" + GAME.encode() + b" synthetic"


def probe_files():
    return [
        (GAME + "DATA00/PARAM.SFO", sfo()),
        (GAME + "DATA00/DATA.BIN", b"A" * 4096),
        (GAME + "DATA01/PARAM.SFO", sfo()),
        (GAME + "DATA01/DATA.BIN", b"slot one"),
    ]


def sigil_unit(files):
    """A STORED zip of files only, sorted by name, dated 1980-01-01 00:00, no extra fields."""
    files = sorted(files)
    local, central = b"", b""
    for name, data in files:
        raw = name.encode("ascii")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        offset = len(local)
        local += struct.pack("<IHHHHHIIIHH", 0x04034B50, VERSION_20, 0, 0, 0, DOS_DATE_1980_01_01,
                             crc, len(data), len(data), len(raw), 0) + raw + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, VERSION_20, VERSION_20, 0, 0, 0,
                               DOS_DATE_1980_01_01, crc, len(data), len(data), len(raw), 0, 0, 0,
                               0, 0, offset) + raw
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(files), len(files), len(central),
                      len(local), 0)
    return local + central + end


def folder_entries(files):
    folders = []
    for name, _ in files:
        folder = name.split("/")[0] + "/"
        if folder not in folders:
            folders.append(folder)
    return folders


def deflated_unit(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as z:
        for folder in folder_entries(files):
            z.writestr(zipfile.ZipInfo(folder, (1980, 1, 1, 0, 0, 0)), b"")
        for name, data in reversed(files):
            info = zipfile.ZipInfo(name, (2026, 10, 11, 18, 15, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    return buffer.getvalue()


class Unseekable(io.RawIOBase):
    """A pipe-like sink: zipfile then writes a data descriptor after each entry."""

    def __init__(self):
        self.data = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.data += b
        return len(b)


def descriptor_unit(files):
    sink = Unseekable()
    with zipfile.ZipFile(sink, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, (2026, 10, 11, 18, 15, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            with z.open(info, "w") as entry:
                entry.write(data)
    return bytes(sink.data)


def romm_hash(zip_bytes):
    """RomM 5.4.0's hash_zip_contents (backend/handler/filesystem/assets_handler.py): the names
    sorted, folder entries skipped, then MD5 of the "name:md5" lines joined by newlines."""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        lines = [name + ":" + hashlib.md5(z.read(name)).hexdigest()
                 for name in sorted(z.namelist()) if not name.endswith("/")]
    return hashlib.md5("\n".join(lines).encode()).hexdigest()


def name_order_hashes():
    """Names where sorting the names (RomM) and sorting the lines differ: ':' sorts after '.' and
    the digits, so "DATA:" comes after "DATA.BIN:" and "DATA1:"."""
    files = [(GAME + "DATA00/DATA", b"one"), (GAME + "DATA00/DATA.BIN", b"two"),
             (GAME + "DATA00/DATA1", b"three")]
    lines = [name + ":" + hashlib.md5(data).hexdigest() for name, data in sorted(files)]
    by_name = hashlib.md5("\n".join(lines).encode()).hexdigest()
    by_line = hashlib.md5("\n".join(sorted(lines)).encode()).hexdigest()
    return romm_hash(sigil_unit(files)), by_name, by_line


def main():
    out = sys.argv[1]
    files = probe_files()
    units = {
        "probe-stored.zip": sigil_unit(files),
        "probe-deflated.zip": deflated_unit(files),
        "probe-descriptors.zip": descriptor_unit(files),
    }
    for name, data in units.items():
        digest = romm_hash(data)
        assert digest == EXPECTED_HASH, (name, digest)
        with open(f"{out}/{name}", "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes, content_hash {digest}")
    romm, by_name, by_line = name_order_hashes()
    assert romm == by_name != by_line
    print(f"name order: RomM {romm}, lines sorted instead {by_line}")


if __name__ == "__main__":
    main()
