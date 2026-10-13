"""Seed the integration RomM: admin user, synthetic PSP files, a library scan, a client token.

Runs inside the RomM container with RomM's own Python (`scripts/dev.sh romm-up` does this), so it
needs nothing installed elsewhere: the standard library plus the python-socketio client RomM ships.
The scan is started over RomM's socket, the way its web UI does, because RomM has no REST endpoint
for it. Prints one JSON object on stdout for the tests; the token in it is a secret.

The payload ROM gets a synthetic PNG cover, uploaded the way RomM's web UI uploads custom artwork;
the seed has no metadata providers, so it is the only cover. It also gets a synthetic description
and facts (genres, developer, publisher, release date), entered as a player edits a game in the web
UI, for the details screen. RomM keeps the upload as the large
cover and resizes it into the small one Skiff shows. The extra ROM gets none: every ROM update in
RomM (5.3 and 5.4) also cleans the file's name on disk, which would drop the '+' its name is there for.

With SKIFF_LIBRARY_ROMS set, the platform also gets a library for the app's hardware sessions (A1,
A2): that many small numbered ROMs (several pages), every third with a PNG cover of its own and
Skiff Library 02 with a JPEG one; a file the app cannot install (.zip), a name with a control
character and a name too long for the Memory Stick. Files already in the platform's folder are
scanned too: scripts/dev.sh copies the launch check's disc images there first
(tests/hardware/make_launch_disc.py). From its ISO the seed makes Skiff Launch Check CHD.chd, which
RomM converts on download (compose.yaml turns that on), so the PSP launches a CSO rom-converto
made.

The admin's favourites are kept as RomM's web UI keeps them, in a private collection marked as
favourites: the payload ROM, and with SKIFF_LIBRARY_ROMS also the first LIBRARY_FAVOURITES numbered
ROMs and the three special files, plus a second favourites collection with LIBRARY_MORE_FAVOURITES
more (a user may have several; RomM lists their union). Adding a ROM to a collection renames
nothing, so any ROM may be a favourite.

Save sync: unless SKIFF_SAVE_DISC is 0 (the fixture recording turns it off, so the recorded lists
keep their two ROMs), a third scan adds a synthetic PSP disc, an ISO 9660 image holding only
UMD_DATA.BIN with the made-up game ID SKIF-00002 (no game, no EBOOT): RomM 5.4 reads its save ID from
it (save_target SKIF00002, layout folder-prefix), which the app's save sync and the integration tests
use.

A game of another platform: unless SKIFF_FOREIGN_ROM is 0, a scan adds one small synthetic file
to a second platform (gba). RomM lets a player send any platform's game to the PSP ("Install on
device"), so the install tests send it and Skiff refuses it. Scanned after the PSP's ROMs above, so
they keep their ids.

Download conversion (compose.yaml turns it on): unless SKIFF_CONVERT_DISCS is 0, a last scan adds two
synthetic PSP discs stored as CHD, which a PSP cannot run and RomM converts on download
(`?format=cso`): ISO 9660 images like the save disc's (made-up IDs SKIF-00003 and SKIF-00004), with
seeded bytes and zeros after the file so a CSO holds both kinds of block, compressed to DVD-mode CHDs
by the rom-converto RomM ships. RomM converts the first in well under a second; the second's
conversion is held back by rom-converto-delay.sh, so a download that starts it is answered 202. The
seed checks RomM offers both as cso, zso and iso, and read their save IDs from the CHDs (SKIF00003,
SKIF00004, layout folder-prefix), as from the save disc. `python3 /seed.py convert-discs` adds only them,
to a server seeded before (scripts/dev.sh romm-record, after the other fixtures are recorded).

Collections, for the Collections screen: the admin keeps a regular collection holding both seeded
ROMs (with SKIFF_LIBRARY_ROMS also the first LIBRARY_SHELF numbered ROMs and the three special
files, two pages), an empty one, and a smart one RomM fills from a saved search (the extra ROM). A
second user, a viewer, keeps a public collection holding the extra ROM, which RomM lists to the
admin too, and a private one, which it lists to nobody else. A second client token can read
collections; the first cannot (RomM answers it 403).

Environment: SKIFF_ADMIN_USER, SKIFF_ADMIN_PASSWORD, SKIFF_PAYLOAD_BYTES (optional),
SKIFF_LIBRARY_ROMS (optional), SKIFF_SAVE_DISC (optional, 1 by default), SKIFF_FOREIGN_ROM
(optional, 1 by default), SKIFF_CONVERT_DISCS (optional, 1 by default).
"""

import asyncio
import base64
import io
import random
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

import shutil
import subprocess
import tempfile

import socketio
from PIL import Image, ImageDraw

API = "http://localhost:8080"
PLATFORM_SLUG = "psp"
ROMS_DIR = Path("/romm/library/roms")
LIBRARY_DIR = ROMS_DIR / PLATFORM_SLUG
PAYLOAD_NAME = "Skiff Test Payload.iso"
DEFAULT_PAYLOAD_BYTES = 1024 * 1024
# A second, small file: a second ROM for pagination, and a name that only downloads when every
# reserved character in it is percent-encoded ('#' would end the path, '&' and '+' change its
# meaning, 'é' is two UTF-8 bytes).
EXTRA_NAME = "Skiff Extra #2 (Café & Co+).iso"
EXTRA_BYTES = 1536
EXTRA_SEED = b"skiff-integration-extra"
# The A1 library (SKIFF_LIBRARY_ROMS): numbered ROMs of a few KiB each, sizes cycling so the size
# column varies, each with its own content; a .zip, which the app lists but cannot install; a name
# with a control character (BEL), which it shows with '?' and as not downloadable
# (SKIFF_ROMM_NAME_CONTROL_CHAR); and a name longer than the 127 bytes a Memory Stick file name may
# take, which it shortens for the file it downloads (src/storage/).
LIBRARY_NAME_FORMAT = "Skiff Library {index:02d}.iso"
LIBRARY_BASE_BYTES = 4096
LIBRARY_SIZE_STEPS = 5
LIBRARY_SEED = b"skiff-integration-library"
UNSUPPORTED_NAME = "Skiff Not A Game.zip"
CONTROL_NAME = "Skiff Control\aCharacter.iso"
LONG_NAME = "Skiff " + "A Name Far Too Long For The Memory Stick " * 4 + "End.iso"
SPECIAL_BYTES = 2048
# The synthetic save disc: its file, the ID its UMD_DATA.BIN starts with (as a disc writes it, with
# the dash), and the save ID RomM's scan makes of it.
SAVE_DISC_NAME = "Skiff Save Disc.iso"
SAVE_DISC_ID = "SKIF-00002"
SAVE_DISC_TARGET = "SKIF00002"
SAVE_DISC_LAYOUT = "folder-prefix"
# ISO 9660 (ECMA-119): 2048-byte sectors, the system area (16 sectors), then the volume
# descriptors; the path tables, the root folder and the one file after them.
ISO_SECTOR = 2048
ISO_PVD_SECTOR = 16
ISO_TERMINATOR_SECTOR = 17
ISO_L_PATH_SECTOR = 18
ISO_M_PATH_SECTOR = 19
ISO_ROOT_SECTOR = 20
ISO_FILE_SECTOR = 21
ISO_ROOT_RECORD_AT = 156
ISO_DIRECTORY_FLAG = 2
# The game of another platform (SKIFF_FOREIGN_ROM): synthetic bytes under a name of that platform.
FOREIGN_SLUG = "gba"
FOREIGN_NAME = "Skiff Foreign Game.gba"
FOREIGN_BYTES = 2048
FOREIGN_SEED = b"skiff-integration-foreign"
# The discs RomM converts on download (SKIFF_CONVERT_DISCS): file, game ID. The slow one's name is the
# one rom-converto-delay.sh waits on.
CONVERT_DISCS = (
    ("Skiff Convert Disc", "SKIF-00003"),
    ("Skiff Convert Slow", "SKIF-00004"),
)
# 32 sectors: the ISO 9660 structure, then seeded bytes (blocks a CSO stores as they are), then
# zeros (blocks it deflates).
CONVERT_DISC_SECTORS = 32
CONVERT_NOISE_SECTORS = 4
CONVERT_SEED = b"skiff-integration-convert"
# The launch check's disc (scripts/dev.sh copies it in), and the CHD made of it for the PSP to get
# converted.
LAUNCH_ISO_NAME = "Skiff Launch Check.iso"
LAUNCH_CHD_NAME = "Skiff Launch Check CHD.chd"
# The rom-converto RomM ships, by its own path: rom-converto-delay.sh stands in front of it on PATH.
ROM_CONVERTO = "/usr/bin/rom-converto"
# What RomM must offer a converted disc's download as (DetailedRomSchema.download_formats).
CONVERT_FORMATS = {"cso", "zso", "iso"}
CONVERT_TIMEOUT_SECONDS = 60
# ROMs per listing page when the seed looks them up after the scan.
LIST_LIMIT = 500
# Synthetic, reproducible content: SHA-256 in counter mode over this seed. Same size, same bytes,
# same hashes on every run and every machine.
PAYLOAD_SEED = b"skiff-integration-payload"
SCAN_TIMEOUT_SECONDS = 120
# Synthetic covers: 600x800, so RomM's small cover is 240x320 (it scales covers under 1000 px tall
# by 0.4), a size real covers have. Gradients with seeded noise compress about as badly as a
# photograph, so the bytes are realistic too, and the same on every run.
COVER_SIZE = (600, 800)
COVER_NOISE = 32
COVER_SEED = 2026
# The hardware library (SKIFF_LIBRARY_ROMS): every third numbered ROM gets a PNG cover of its own,
# and one a JPEG cover, which Skiff shows as a placeholder (it decodes PNG only).
LIBRARY_COVER_EVERY = 3
LIBRARY_JPEG_COVER_INDEX = 2
COVER_LABEL = "SKIFF SYNTHETIC COVER"
COVER_LABEL_BOX = (60, 60, 540, 200)
COVER_LABEL_AT = (80, 110)
MULTIPART_BOUNDARY = "skiff-seed-cover"
# The payload's description and facts, set as a player edits a game in RomM's web UI (summary and
# manual metadata; RomM 5.4 merges the manual metadata into metadatum). Synthetic text, long enough
# to scroll on the details screen, with the typographic punctuation Skiff makes ASCII and a
# paragraph break. RomM keeps manual dates in Unix milliseconds: 2010-01-01T00:00:00Z.
PAYLOAD_SUMMARY = (
    "Skiff Test Payload is not a game: the integration seed makes up its bytes, its cover and this "
    "text, so Skiff\u2019s details screen has something to show.\n\n"
    "This second paragraph is here to make the description scroll on a PSP \u2014 four or five "
    "lines beside the cover are not enough for it. Its \u201cquotes\u201d, its dash and its "
    "ellipsis\u2026 come out as plain ASCII.\n\n"
    "A third paragraph closes it. Nothing here comes from a real game, a metadata provider or a "
    "player."
)
PAYLOAD_MANUAL_METADATA = {
    "genres": ["Puzzle", "Simulator"],
    "developers": ["Skiff Test Studio"],
    "publishers": ["Skiff Test Publishing"],
    "first_release_date": 1262304000000,
}
# The favourites (see the module's docstring). The first collection is named as the web UI names the
# one it creates on a player's first favourite.
FAVOURITES_NAME = "Favorites"
MORE_FAVOURITES_NAME = "Skiff More Favourites"
LIBRARY_FAVOURITES = 30
LIBRARY_MORE_FAVOURITES = 2
# The collections (see the module's docstring).
SHELF_NAME = "Skiff Shelf"
EMPTY_SHELF_NAME = "Skiff Empty Shelf"
SMART_SHELF_NAME = "Skiff Smart Shelf"
SMART_SHELF_CRITERIA = {"search_term": "Extra"}
VIEWER_USER = "skiffviewer"
VIEWER_PUBLIC_NAME = "Skiff Viewer Shelf"
VIEWER_PRIVATE_NAME = "Skiff Viewer Private"
LIBRARY_SHELF = 30
VIEWER_PASSWORD_BYTES = 16
# Every call is bounded, so a stalled RomM fails the seed instead of hanging it.
REQUEST_TIMEOUT_SECONDS = 30
TOKEN_NAME = "skiff-integration"
COLLECTIONS_TOKEN_NAME = "skiff-integration-collections"
# What a Skiff client needs: browse and download, upload saves, register itself as a device.
TOKEN_SCOPES = [
    "me.read",
    "roms.read",
    "roms.user.read",
    "platforms.read",
    "assets.read",
    "assets.write",
    "devices.read",
    "devices.write",
]
# The same, and the optional scope the Collections screen needs.
COLLECTIONS_TOKEN_SCOPES = TOKEN_SCOPES + ["collections.read"]
HTTP_OK = 200
HTTP_CREATED = 201
HTTP_FORBIDDEN = 403


def log(message):
    print(f"seed: {message}", file=sys.stderr, flush=True)


def write_payload(size, name=PAYLOAD_NAME, seed=PAYLOAD_SEED, folder=LIBRARY_DIR):
    """Create a payload once and return its size and hashes (the ones RomM records)."""
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / name
    sha1, md5, crc = hashlib.sha1(), hashlib.md5(), 0
    with path.open("wb") as out:
        written, counter = 0, 0
        while written < size:
            block = hashlib.sha256(seed + counter.to_bytes(8, "big")).digest()
            block = block[: size - written]
            out.write(block)
            sha1.update(block)
            md5.update(block)
            crc = zlib.crc32(block, crc)
            written += len(block)
            counter += 1
    return {
        "file_name": name,
        "size": size,
        "sha1": sha1.hexdigest(),
        "md5": md5.hexdigest(),
        "crc32": f"{crc:08x}",
    }


def iso_both16(value):
    return value.to_bytes(2, "little") + value.to_bytes(2, "big")


def iso_both32(value):
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def iso_text(text, size):
    return text.encode("ascii").ljust(size, b" ")


def iso_record(name, sector, size, flags):
    """One directory record (ECMA-119 9.1), padded to an even length."""
    record = bytearray(33 + len(name))
    record[2:10] = iso_both32(sector)
    record[10:18] = iso_both32(size)
    record[18:25] = bytes([126, 1, 1, 0, 0, 0, 0])  # 2026-01-01 00:00:00 UTC
    record[25] = flags
    record[28:32] = iso_both16(1)
    record[32] = len(name)
    record[33:] = name
    if len(record) % 2:
        record += b"\0"
    record[0] = len(record)
    return bytes(record)


def disc_image(disc_id, volume_name, sectors=ISO_FILE_SECTOR + 1):
    """A synthetic PSP disc: just enough ISO 9660 for a reader to find UMD_DATA.BIN in its root
    folder, the file RomM's scan reads the game ID from ("SKIF-00002|..."), in an image of `sectors`
    sectors (zeros after the file)."""
    umd_data = f"{disc_id}|0000000000000000|0001|G".encode("ascii")
    image = bytearray(sectors * ISO_SECTOR)
    root = iso_record(b"\0", ISO_ROOT_SECTOR, ISO_SECTOR, ISO_DIRECTORY_FLAG)
    folder = root + iso_record(b"\1", ISO_ROOT_SECTOR, ISO_SECTOR, ISO_DIRECTORY_FLAG)
    folder += iso_record(b"UMD_DATA.BIN;1", ISO_FILE_SECTOR, len(umd_data), 0)
    path_entry = bytes([1, 0]) + ISO_ROOT_SECTOR.to_bytes(4, "little") + (1).to_bytes(2, "little")
    path_table_l = path_entry + b"\0\0"
    path_table_m = bytes([1, 0]) + ISO_ROOT_SECTOR.to_bytes(4, "big") + (1).to_bytes(2, "big")
    path_table_m += b"\0\0"
    pvd = bytearray(ISO_SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = iso_text("PSP GAME", 32)
    pvd[40:72] = iso_text(volume_name, 32)
    pvd[80:88] = iso_both32(sectors)
    pvd[120:124] = iso_both16(1)
    pvd[124:128] = iso_both16(1)
    pvd[128:132] = iso_both16(ISO_SECTOR)
    pvd[132:140] = iso_both32(len(path_table_l))
    pvd[140:144] = ISO_L_PATH_SECTOR.to_bytes(4, "little")
    pvd[148:152] = ISO_M_PATH_SECTOR.to_bytes(4, "big")
    pvd[ISO_ROOT_RECORD_AT : ISO_ROOT_RECORD_AT + len(root)] = root
    pvd[190:318] = iso_text("", 128)
    pvd[318:574] = iso_text("", 256)
    pvd[574:702] = iso_text("PSP GAME", 128)
    pvd[702:813] = iso_text("", 111)
    for at in (813, 830, 847, 864):
        pvd[at : at + 17] = b"0" * 16 + b"\0"
    pvd[881] = 1
    terminator = bytearray(ISO_SECTOR)
    terminator[0:7] = b"\xffCD001\x01"
    for sector, data in (
        (ISO_PVD_SECTOR, pvd),
        (ISO_TERMINATOR_SECTOR, terminator),
        (ISO_L_PATH_SECTOR, path_table_l),
        (ISO_M_PATH_SECTOR, path_table_m),
        (ISO_ROOT_SECTOR, folder),
        (ISO_FILE_SECTOR, umd_data),
    ):
        image[sector * ISO_SECTOR : sector * ISO_SECTOR + len(data)] = data
    return image


def write_save_disc():
    """The save disc (see the module's docstring)."""
    LIBRARY_DIR.mkdir(parents=True, exist_ok=True)
    (LIBRARY_DIR / SAVE_DISC_NAME).write_bytes(bytes(disc_image(SAVE_DISC_ID, "SKIFF_SAVE_DISC")))
    return SAVE_DISC_NAME


def hashes(data):
    return {
        "size": len(data),
        "sha1": hashlib.sha1(data).hexdigest(),
        "md5": hashlib.md5(data).hexdigest(),
        "crc32": f"{zlib.crc32(data):08x}",
    }


def write_convert_disc(name, disc_id):
    """One disc RomM converts (see the module's docstring), compressed to a CHD in the platform's
    folder; returns the CHD's name and hashes and the ISO's (what a conversion must give back)."""
    image = disc_image(disc_id, name.upper().replace(" ", "_"), CONVERT_DISC_SECTORS)
    noise_at = (ISO_FILE_SECTOR + 1) * ISO_SECTOR
    noise = b"".join(
        hashlib.sha256(CONVERT_SEED + disc_id.encode() + counter.to_bytes(8, "big")).digest()
        for counter in range(CONVERT_NOISE_SECTORS * ISO_SECTOR // 32)
    )
    image[noise_at : noise_at + len(noise)] = noise
    chd_name = f"{name}.chd"
    with tempfile.TemporaryDirectory() as work:
        iso = Path(work) / f"{name}.iso"
        iso.write_bytes(bytes(image))
        compress_to_chd(iso, chd_name)
    stored = hashes((LIBRARY_DIR / chd_name).read_bytes())
    return {"file_name": chd_name, **stored, "iso": hashes(bytes(image))}


def compress_to_chd(iso, chd_name):
    """The disc image at iso compressed to a DVD-mode CHD named chd_name in the platform's folder,
    by the rom-converto RomM ships."""
    LIBRARY_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        chd = Path(work) / chd_name
        subprocess.run(
            [ROM_CONVERTO, "--no-update-check", "chd", "compress", "--dvd", "--quiet", str(iso),
             str(chd)],
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=sys.stderr,
            timeout=CONVERT_TIMEOUT_SECONDS,
        )
        shutil.move(chd, LIBRARY_DIR / chd_name)


def convert_discs(auth, cookie):
    """The discs RomM converts, scanned, each checked offered as CONVERT_FORMATS; returns them by
    role: "fast" and "slow"."""
    discs = [write_convert_disc(name, disc_id) for name, disc_id in CONVERT_DISCS]
    asyncio.run(scan(cookie))
    _, ids = find_roms(auth, [disc["file_name"] for disc in discs])
    for (_, disc_id), disc, rom_id in zip(CONVERT_DISCS, discs, ids):
        status, rom = request("GET", f"/api/roms/{rom_id}", auth)
        offered = set((rom or {}).get("download_formats") or [])
        if status != HTTP_OK or not CONVERT_FORMATS <= offered:
            raise SystemExit(
                f"seed: RomM offers {disc['file_name']} as {sorted(offered)} (HTTP {status}), not "
                f"{sorted(CONVERT_FORMATS)}: is download conversion on (compose.yaml, config.yml)?"
            )
        save_target = disc_id.replace("-", "")
        if rom.get("save_target") != save_target or rom.get("save_target_layout") != SAVE_DISC_LAYOUT:
            raise SystemExit(
                f"seed: {disc['file_name']} scanned as save_target {rom.get('save_target')!r}, layout "
                f"{rom.get('save_target_layout')!r}, not {save_target!r}"
            )
        disc["rom_id"] = rom_id
        disc["save_target"] = save_target
        log(f"convert disc: ROM {rom_id}, {disc['file_name']} ({disc['size']} bytes, ISO "
            f"{disc['iso']['size']}), offered as {sorted(offered)}")
    return dict(zip(("fast", "slow"), discs))


def save_disc_rom(auth, platform_id):
    """The save disc's ROM, which RomM's scan must have given its save ID."""
    _, (rom_id,) = find_roms(auth, [SAVE_DISC_NAME])
    status, rom = request("GET", f"/api/roms/{rom_id}", auth)
    if (
        status != HTTP_OK
        or rom.get("save_target") != SAVE_DISC_TARGET
        or rom.get("save_target_layout") != SAVE_DISC_LAYOUT
        or rom.get("platform_id") != platform_id
    ):
        raise SystemExit(
            f"seed: the save disc scanned as save_target {rom.get('save_target')!r}, layout "
            f"{rom.get('save_target_layout')!r} (HTTP {status})"
        )
    return {"rom_id": rom_id, "file_name": SAVE_DISC_NAME, "save_target": SAVE_DISC_TARGET}


def request(method, path, auth, body=None):
    """One API call; returns (status, parsed JSON or None). `auth` is a full Authorization value."""
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"{API}{path}", data=data, method=method)
    req.add_header("Authorization", auth)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as error:
        return error.code, None


def cover_image(seed=COVER_SEED, image_format="PNG"):
    """A synthetic cover (PNG or JPEG), the same bytes on every run for a seed."""
    noise = random.Random(seed)
    width, height = COVER_SIZE
    image = Image.new("RGB", COVER_SIZE)
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            n = noise.randrange(COVER_NOISE)
            pixels[x, y] = (
                min(255, x * 220 // (width - 1) + n),
                min(255, y * 220 // (height - 1) + n),
                ((x ^ y) & 127) + n,
            )
    draw = ImageDraw.Draw(image)
    draw.rectangle(COVER_LABEL_BOX, fill=(250, 250, 250))
    draw.text(COVER_LABEL_AT, COVER_LABEL, fill=(0, 0, 0))
    out = io.BytesIO()
    image.save(out, image_format)
    return out.getvalue()


def set_cover(auth, rom_id, fs_name, seed=COVER_SEED, jpeg=False):
    """Upload a synthetic cover as the ROM's custom artwork; returns RomM's small cover path. RomM
    also cleans the ROM's file name on disk, so only a ROM whose name is already clean may get one."""
    extension, content_type = ("jpg", "image/jpeg") if jpeg else ("png", "image/png")
    body = (
        f"--{MULTIPART_BOUNDARY}\r\n"
        f'Content-Disposition: form-data; name="artwork"; filename="cover.{extension}"\r\n'
        f"Content-Type: {content_type}\r\n\r\n"
    ).encode() + cover_image(seed, "JPEG" if jpeg else "PNG") + f"\r\n--{MULTIPART_BOUNDARY}--\r\n".encode()
    req = urllib.request.Request(f"{API}/api/roms/{rom_id}", data=body, method="PUT")
    req.add_header("Authorization", auth)
    req.add_header("Content-Type", f"multipart/form-data; boundary={MULTIPART_BOUNDARY}")
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            rom = json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise SystemExit(f"seed: uploading a cover for ROM {rom_id} failed with HTTP {error.code}")
    if rom.get("fs_name") != fs_name:
        raise SystemExit(f"seed: uploading a cover renamed {fs_name!r} to {rom.get('fs_name')!r}")
    path = rom.get("path_cover_small") or ""
    if not path:
        raise SystemExit(f"seed: ROM {rom_id} has no small cover after the upload")
    log(f"ROM {rom_id} cover: {path}")
    return path


def post_form(auth, path, fields, method="POST"):
    """A multipart form request (a POST, the way RomM's web UI creates collections, or a PUT, the way
    it edits a game); returns the parsed answer."""
    body = "".join(
        f"--{MULTIPART_BOUNDARY}\r\n"
        f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
        f"{value}\r\n"
        for name, value in fields.items()
    ).encode() + f"--{MULTIPART_BOUNDARY}--\r\n".encode()
    req = urllib.request.Request(f"{API}{path}", data=body, method=method)
    req.add_header("Authorization", auth)
    req.add_header("Content-Type", f"multipart/form-data; boundary={MULTIPART_BOUNDARY}")
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise SystemExit(f"seed: {method} {path} ({fields.get('name')!r}) failed with HTTP {error.code}")


def set_details(auth, rom_id, fs_name):
    """The payload's description and facts, as RomM's web UI edits a game (see PAYLOAD_SUMMARY). An
    edit also cleans the file name on disk, so only a ROM whose name is already clean may get it."""
    summary = PAYLOAD_SUMMARY
    rom = post_form(
        auth,
        f"/api/roms/{rom_id}",
        {"summary": summary, "raw_manual_metadata": json.dumps(PAYLOAD_MANUAL_METADATA)},
        method="PUT",
    )
    if rom.get("fs_name") != fs_name:
        raise SystemExit(f"seed: editing ROM {rom_id} renamed {fs_name!r} to {rom.get('fs_name')!r}")
    if rom.get("summary") != summary:
        raise SystemExit(f"seed: ROM {rom_id} kept no description")
    metadatum = rom.get("metadatum") or {}
    for key, value in PAYLOAD_MANUAL_METADATA.items():
        if metadatum.get(key) != value:
            raise SystemExit(f"seed: ROM {rom_id} metadatum {key} is {metadatum.get(key)!r}, not {value!r}")
    log(f"ROM {rom_id} details: {len(summary)} characters of description, {len(metadatum)} facts")


def create_collection(auth, name, rom_ids, public=False, favourite=False):
    """A collection holding rom_ids; returns its id."""
    query = f"is_public={str(public).lower()}" + ("&is_favorite=true" if favourite else "")
    collection = post_form(auth, f"/api/collections?{query}", {"name": name})
    if rom_ids:
        status, _ = request("POST", f"/api/collections/{collection['id']}/roms", auth, {"rom_ids": rom_ids})
        if status != HTTP_OK:
            raise SystemExit(f"seed: adding ROMs to {name!r} failed with HTTP {status}")
    log(f"{'favourites' if favourite else 'collection'} {name!r} (collection {collection['id']}, "
        f"{'public' if public else 'private'}): {len(rom_ids)} ROMs")
    return collection["id"]


def create_favourites(auth, name, rom_ids):
    """A private favourites collection holding rom_ids, created the way RomM's web UI creates one."""
    create_collection(auth, name, rom_ids, favourite=True)


def create_smart_collection(auth, name, criteria):
    """A private smart collection RomM fills from criteria; returns its id."""
    collection = post_form(auth, "/api/collections/smart?is_public=false",
                           {"name": name, "filter_criteria": json.dumps(criteria)})
    log(f"smart collection {name!r} (smart collection {collection['id']}): "
        f"{collection.get('rom_count')} ROMs")
    return collection["id"]


def create_viewer(auth):
    """A second RomM user with the viewer role; returns its Basic authorisation."""
    password = os.urandom(VIEWER_PASSWORD_BYTES).hex()
    status, _ = request(
        "POST",
        "/api/users",
        auth,
        {"username": VIEWER_USER, "email": f"{VIEWER_USER}@example.invalid", "password": password,
         "role": "viewer"},
    )
    if status != HTTP_CREATED:
        raise SystemExit(f"seed: creating the viewer failed with HTTP {status}")
    return basic_auth(VIEWER_USER, password)


def set_collections(auth, rom_id, extra_rom_id, library, library_roms):
    """The collections (see the module's docstring); returns their ids by role."""
    shelf = [rom_id, extra_rom_id]
    if library:
        numbered, special = library[:library_roms], library[library_roms:]
        _, shelf_ids = find_roms(auth, numbered[:LIBRARY_SHELF] + special)
        shelf += shelf_ids
    ids = {
        "shelf": create_collection(auth, SHELF_NAME, shelf),
        "empty": create_collection(auth, EMPTY_SHELF_NAME, []),
        "smart": create_smart_collection(auth, SMART_SHELF_NAME, SMART_SHELF_CRITERIA),
    }
    viewer = create_viewer(auth)
    ids["viewer_public"] = create_collection(viewer, VIEWER_PUBLIC_NAME, [extra_rom_id], public=True)
    ids["viewer_private"] = create_collection(viewer, VIEWER_PRIVATE_NAME, [rom_id])
    ids["shelf_roms"] = len(shelf)
    return ids


def set_favourites(auth, payload_rom_id, library, library_roms):
    """The admin's favourites (see the module's docstring); returns their ROM ids."""
    favourites = [payload_rom_id]
    more = []
    if library:
        numbered, special = library[:library_roms], library[library_roms:]
        _, favourites_ids = find_roms(auth, numbered[:LIBRARY_FAVOURITES] + special)
        favourites += favourites_ids
        more_names = numbered[LIBRARY_FAVOURITES : LIBRARY_FAVOURITES + LIBRARY_MORE_FAVOURITES]
        if more_names:
            _, more = find_roms(auth, more_names)
    create_favourites(auth, FAVOURITES_NAME, favourites)
    if more:
        create_favourites(auth, MORE_FAVOURITES_NAME, more)
    return favourites + more


def basic_auth(user, password):
    return "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()


def create_admin(user, password, auth):
    status, _ = request(
        "POST",
        "/api/users",
        auth,
        {"username": user, "email": f"{user}@example.invalid", "password": password, "role": "admin"},
    )
    if status == HTTP_CREATED:
        log(f"created admin {user}")
    elif status == HTTP_FORBIDDEN:
        log("an admin already exists, reusing it")
    else:
        raise SystemExit(f"seed: creating the admin failed with HTTP {status}")


def session_cookie(auth):
    """Log in with a session, which RomM's socket uses to authorise a scan."""
    req = urllib.request.Request(f"{API}/api/login", method="POST")
    req.add_header("Authorization", auth)
    with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT_SECONDS) as response:
        cookies = response.headers.get_all("Set-Cookie") or []
    return "; ".join(cookie.split(";", 1)[0] for cookie in cookies)


async def scan(cookie, slug=PLATFORM_SLUG):
    client = socketio.AsyncClient()
    outcome = asyncio.get_running_loop().create_future()

    @client.on("scan:done")
    async def done(stats):
        outcome.set_result(stats)

    @client.on("scan:done_ko")
    async def failed(reason):
        outcome.set_exception(SystemExit(f"seed: scan failed: {reason}"))

    await client.connect(
        API,
        headers={"Cookie": cookie},
        socketio_path="/ws/socket.io",
        wait_timeout=REQUEST_TIMEOUT_SECONDS,
    )
    try:
        await client.emit("scan", {"platform_fs_slugs": [slug], "type": "quick", "apis": []})
        stats = await asyncio.wait_for(outcome, SCAN_TIMEOUT_SECONDS)
        log(f"scan done: {stats}")
    finally:
        await client.disconnect()


def write_library(count):
    """The A1 library's files; returns their names."""
    names = []
    for index in range(1, count + 1):
        name = LIBRARY_NAME_FORMAT.format(index=index)
        size = LIBRARY_BASE_BYTES * (1 + index % LIBRARY_SIZE_STEPS)
        write_payload(size, name, LIBRARY_SEED + index.to_bytes(4, "big"))
        names.append(name)
    for name in (UNSUPPORTED_NAME, CONTROL_NAME, LONG_NAME):
        write_payload(SPECIAL_BYTES, name, LIBRARY_SEED + name.encode())
        names.append(name)
    if (LIBRARY_DIR / LAUNCH_ISO_NAME).is_file():
        compress_to_chd(LIBRARY_DIR / LAUNCH_ISO_NAME, LAUNCH_CHD_NAME)
        log(f"launch check: {LAUNCH_CHD_NAME} made from {LAUNCH_ISO_NAME}")
    return names


def find_roms(auth, names, slug=PLATFORM_SLUG):
    """The platform's id and the ROM id of each file name, in order; every file in the platform's
    folder must have been scanned."""
    status, platforms = request("GET", "/api/platforms", auth)
    platform = next((p for p in platforms or [] if p["fs_slug"] == slug), None)
    if status != HTTP_OK or platform is None:
        raise SystemExit(f"seed: platform {slug} missing after the scan (HTTP {status})")
    items = []
    while True:
        status, page = request(
            "GET",
            f"/api/roms?platform_ids={platform['id']}&limit={LIST_LIMIT}&offset={len(items)}"
            "&with_char_index=false&with_filter_values=false&with_rom_id_index=false",
            auth,
        )
        batch = (page or {}).get("items", [])
        items += batch
        if status != HTTP_OK or not batch or len(items) >= (page or {}).get("total", 0):
            break
    on_disk = sorted(path.name for path in (ROMS_DIR / slug).iterdir() if path.is_file())
    scanned = sorted(rom["fs_name"] for rom in items)
    if status != HTTP_OK or scanned != on_disk:
        missing = sorted(set(on_disk) - set(scanned))
        raise SystemExit(f"seed: {len(scanned)} of {len(on_disk)} files scanned (HTTP {status}), "
                         f"missing: {missing}")
    ids = []
    for name in names:
        rom = next((r for r in items if r["fs_name"] == name), None)
        if status != HTTP_OK or rom is None:
            raise SystemExit(f"seed: {name} missing after the scan (HTTP {status})")
        ids.append(rom["id"])
    return platform["id"], ids


def set_library_covers(auth, count):
    """Covers for the hardware library's numbered ROMs: PNG on every third, JPEG on one."""
    names = {
        index: LIBRARY_NAME_FORMAT.format(index=index)
        for index in range(1, count + 1)
        if index % LIBRARY_COVER_EVERY == 1 or index == LIBRARY_JPEG_COVER_INDEX
    }
    _, ids = find_roms(auth, list(names.values()))
    for (index, name), rom_id in zip(names.items(), ids):
        set_cover(auth, rom_id, name, COVER_SEED + index, jpeg=index == LIBRARY_JPEG_COVER_INDEX)
    log(f"library covers: {len(names)} ROMs ({LIBRARY_JPEG_COVER_INDEX:02d} as JPEG)")


def create_token(auth, name=TOKEN_NAME, scopes=TOKEN_SCOPES):
    status, token = request(
        "POST", "/api/client-tokens", auth, {"name": name, "scopes": scopes}
    )
    if status != HTTP_CREATED or not token:
        raise SystemExit(f"seed: creating the client token failed with HTTP {status}")
    return token["raw_token"]


def main():
    user = os.environ["SKIFF_ADMIN_USER"]
    password = os.environ["SKIFF_ADMIN_PASSWORD"]
    if sys.argv[1:] == ["convert-discs"]:
        auth = basic_auth(user, password)
        print(json.dumps({"convert": convert_discs(auth, session_cookie(auth))}))
        return
    size = int(os.environ.get("SKIFF_PAYLOAD_BYTES", DEFAULT_PAYLOAD_BYTES))
    library_roms = int(os.environ.get("SKIFF_LIBRARY_ROMS", "0"))
    auth = basic_auth(user, password)

    create_admin(user, password, auth)
    started = time.monotonic()
    # Two scans: one scan numbers its new ROMs in no fixed order, and the recorded fixtures
    # (tests/fixtures/romm/) expect the payload to be ROM 1 and the extra file ROM 2.
    payload = write_payload(size)
    asyncio.run(scan(session_cookie(auth)))
    extra = write_payload(EXTRA_BYTES, EXTRA_NAME, EXTRA_SEED)
    library = write_library(library_roms) if library_roms > 0 else []
    asyncio.run(scan(session_cookie(auth)))
    save_disc = None
    with_save_disc = os.environ.get("SKIFF_SAVE_DISC", "1") != "0"
    if with_save_disc:
        write_save_disc()
        asyncio.run(scan(session_cookie(auth)))
    foreign = None
    if os.environ.get("SKIFF_FOREIGN_ROM", "1") != "0":
        foreign = write_payload(FOREIGN_BYTES, FOREIGN_NAME, FOREIGN_SEED, ROMS_DIR / FOREIGN_SLUG)
        asyncio.run(scan(session_cookie(auth), FOREIGN_SLUG))
        foreign["platform_id"], (foreign["rom_id"],) = find_roms(auth, [FOREIGN_NAME], FOREIGN_SLUG)
        log(f"foreign ROM {foreign['rom_id']} on platform {foreign['platform_id']} ({FOREIGN_SLUG})")
    convert = None
    if os.environ.get("SKIFF_CONVERT_DISCS", "1") != "0":
        convert = convert_discs(auth, session_cookie(auth))
    platform_id, (rom_id, extra_rom_id) = find_roms(auth, [PAYLOAD_NAME, EXTRA_NAME])
    if with_save_disc:
        save_disc = save_disc_rom(auth, platform_id)
        log(f"save disc: ROM {save_disc['rom_id']}, save ID {SAVE_DISC_TARGET}")
    log(f"roms {rom_id}, {extra_rom_id} on platform {platform_id} after {time.monotonic() - started:.1f} s")
    if library:
        log(f"library: {len(library)} more files, and every other file in {LIBRARY_DIR}")
    payload["cover_path"] = set_cover(auth, rom_id, PAYLOAD_NAME)
    set_details(auth, rom_id, PAYLOAD_NAME)
    if library:
        set_library_covers(auth, library_roms)
    favourite_rom_ids = set_favourites(auth, rom_id, library, library_roms)
    collections = set_collections(auth, rom_id, extra_rom_id, library, library_roms)
    extra["rom_id"] = extra_rom_id
    print(
        json.dumps(
            {
                "platform_id": platform_id,
                "rom_id": rom_id,
                "token": create_token(auth),
                **payload,
                "extra": extra,
                "favourite_rom_ids": favourite_rom_ids,
                "collections_token": create_token(auth, COLLECTIONS_TOKEN_NAME, COLLECTIONS_TOKEN_SCOPES),
                "collections": collections,
                "save_disc": save_disc,
                "foreign": foreign,
                "convert": convert,
            }
        )
    )


if __name__ == "__main__":
    main()
