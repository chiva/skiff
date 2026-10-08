"""Pack the launch check (tests/hardware/launch_check.c) as a UMD disc image: an .iso and a .cso.

Runs in the host image (`scripts/dev.sh launch-disc`, after `scripts/dev.sh psp`). The PSP build
lays out the disc's files (UMD_DATA.BIN, PSP_GAME/PARAM.SFO and PSP_GAME/SYSDIR/EBOOT.BIN, see
cmake/SkiffPsp.cmake); this adds the pattern file the check reads back, writes the
ISO with genisoimage and compresses it into a CSO (CISO version 1), which it decompresses again and
compares before keeping it. Everything is synthetic and reproducible except genisoimage's dates.

Usage: python3 make_launch_disc.py <build/psp/pbp/skiff_launch_check>
"""

import struct
import subprocess
import sys
import zlib
from pathlib import Path

ISO_NAME = "Skiff Launch Check.iso"
CSO_NAME = "Skiff Launch Check.cso"
VOLUME_ID = "SKIFF_LAUNCH_CHECK"
# launch_check.c's PATTERN_PATH and PATTERN_BYTES, and its pattern_byte().
PATTERN_PATH = Path("PSP_GAME/USRDIR/PATTERN.BIN")
PATTERN_BYTES = 256 * 1024
SECTOR_SHIFT = 11
PATTERN_STEP = 131
PATTERN_SECTOR_STEP = 37
PATTERN_OFFSET = 7
# The files the PSP build puts in the disc folder.
GAME_FILES = [Path("UMD_DATA.BIN"), Path("PSP_GAME/PARAM.SFO"), Path("PSP_GAME/SYSDIR/EBOOT.BIN")]

# CISO version 1: a 24-byte header, then one 32-bit index entry per block plus one for the end
# (offset >> align; the top bit marks a block stored uncompressed), then the blocks, each raw
# deflate.
CSO_MAGIC = b"CISO"
CSO_HEADER = struct.Struct("<4sIQIBB2x")
CSO_HEADER_SIZE = CSO_HEADER.size
CSO_BLOCK_SIZE = 2048
CSO_VERSION = 1
CSO_ALIGN = 0
CSO_PLAIN_BLOCK = 0x80000000
CSO_INDEX_ENTRY = struct.Struct("<I")
DEFLATE_RAW_WBITS = -15
DEFLATE_LEVEL = 9


def pattern(size):
    return bytes(
        (offset * PATTERN_STEP + (offset >> SECTOR_SHIFT) * PATTERN_SECTOR_STEP + PATTERN_OFFSET)
        & 0xFF
        for offset in range(size)
    )


def write_iso(disc_dir, iso_path):
    subprocess.run(
        [
            "genisoimage", "-quiet", "-iso-level", "4", "-xa", "-A", "PSP GAME", "-sysid",
            "PSP GAME", "-V", VOLUME_ID, "-o", str(iso_path), str(disc_dir),
        ],
        check=True,
    )


def compress_cso(iso):
    blocks = [iso[start : start + CSO_BLOCK_SIZE] for start in range(0, len(iso), CSO_BLOCK_SIZE)]
    offset = CSO_HEADER_SIZE + CSO_INDEX_ENTRY.size * (len(blocks) + 1)
    index, data = [], []
    for block in blocks:
        packer = zlib.compressobj(DEFLATE_LEVEL, zlib.DEFLATED, DEFLATE_RAW_WBITS)
        packed = packer.compress(block) + packer.flush()
        if len(packed) >= len(block):
            index.append(offset | CSO_PLAIN_BLOCK)
            packed = block
        else:
            index.append(offset)
        data.append(packed)
        offset += len(packed)
    index.append(offset)
    header = CSO_HEADER.pack(
        CSO_MAGIC, CSO_HEADER_SIZE, len(iso), CSO_BLOCK_SIZE, CSO_VERSION, CSO_ALIGN
    )
    return header + b"".join(CSO_INDEX_ENTRY.pack(entry) for entry in index) + b"".join(data)


def decompress_cso(cso):
    magic, _, total, block_size, version, align = CSO_HEADER.unpack_from(cso)
    if magic != CSO_MAGIC or version != CSO_VERSION or block_size != CSO_BLOCK_SIZE:
        raise SystemExit("launch disc: the CSO header did not read back")
    count = (total + block_size - 1) // block_size
    entries = [
        CSO_INDEX_ENTRY.unpack_from(cso, CSO_HEADER_SIZE + i * CSO_INDEX_ENTRY.size)[0]
        for i in range(count + 1)
    ]
    out = bytearray()
    for i in range(count):
        start = (entries[i] & ~CSO_PLAIN_BLOCK) << align
        end = (entries[i + 1] & ~CSO_PLAIN_BLOCK) << align
        block = cso[start:end]
        if entries[i] & CSO_PLAIN_BLOCK:
            out += block
        else:
            out += zlib.decompress(block, DEFLATE_RAW_WBITS)
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    out_dir = Path(sys.argv[1])
    disc_dir = out_dir / "disc"
    missing = [str(name) for name in GAME_FILES if not (disc_dir / name).is_file()]
    if missing:
        raise SystemExit(f"launch disc: {', '.join(missing)} missing in {disc_dir}; "
                         "run scripts/dev.sh psp first")
    (disc_dir / PATTERN_PATH).parent.mkdir(parents=True, exist_ok=True)
    (disc_dir / PATTERN_PATH).write_bytes(pattern(PATTERN_BYTES))

    iso_path, cso_path = out_dir / ISO_NAME, out_dir / CSO_NAME
    write_iso(disc_dir, iso_path)
    iso = iso_path.read_bytes()
    cso = compress_cso(iso)
    if decompress_cso(cso) != iso:
        raise SystemExit("launch disc: the CSO does not decompress to the ISO")
    cso_path.write_bytes(cso)
    for path in (iso_path, cso_path):
        print(f"{path}: {path.stat().st_size} bytes, CRC-32 {zlib.crc32(path.read_bytes()):08x}")


if __name__ == "__main__":
    main()
