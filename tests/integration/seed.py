"""Seed the integration RomM: admin user, synthetic PSP files, a library scan, a client token.

Runs inside the RomM container with RomM's own Python (`scripts/dev.sh romm-up` does this), so it
needs nothing installed elsewhere: the standard library plus the python-socketio client RomM ships.
The scan is started over RomM's socket, the way its web UI does, because RomM has no REST endpoint
for it. Prints one JSON object on stdout for the tests; the token in it is a secret.

With SKIFF_LIBRARY_ROMS set, the platform also gets a library for the app's hardware session (A1):
that many small numbered ROMs (several pages), a file the app cannot install (.zip) and a name too
long for the Memory Stick. Files already in the platform's folder are scanned too: scripts/dev.sh
copies the launch check's disc images there first (tests/hardware/make_launch_disc.py).

Environment: SKIFF_ADMIN_USER, SKIFF_ADMIN_PASSWORD, SKIFF_PAYLOAD_BYTES (optional),
SKIFF_LIBRARY_ROMS (optional).
"""

import asyncio
import base64
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

import socketio

API = "http://localhost:8080"
PLATFORM_SLUG = "psp"
LIBRARY_DIR = Path("/romm/library/roms") / PLATFORM_SLUG
PAYLOAD_NAME = "Skiff Test Payload.iso"
DEFAULT_PAYLOAD_BYTES = 1024 * 1024
# A second, small file: a second ROM for pagination, and a name that only downloads when every
# reserved character in it is percent-encoded ('#' would end the path, '&' and '+' change its
# meaning, 'é' is two UTF-8 bytes).
EXTRA_NAME = "Skiff Extra #2 (Café & Co+).iso"
EXTRA_BYTES = 1536
EXTRA_SEED = b"skiff-integration-extra"
# The A1 library (SKIFF_LIBRARY_ROMS): numbered ROMs of a few KiB each, sizes cycling so the size
# column varies, each with its own content; a .zip, which the app lists but cannot install; and a
# name longer than the 127 bytes a Memory Stick file name may take (src/storage/), which the app
# shows as not downloadable.
LIBRARY_NAME_FORMAT = "Skiff Library {index:02d}.iso"
LIBRARY_BASE_BYTES = 4096
LIBRARY_SIZE_STEPS = 5
LIBRARY_SEED = b"skiff-integration-library"
UNSUPPORTED_NAME = "Skiff Not A Game.zip"
LONG_NAME = "Skiff " + "A Name Far Too Long For The Memory Stick " * 4 + "End.iso"
SPECIAL_BYTES = 2048
# ROMs per listing page when the seed looks them up after the scan.
LIST_LIMIT = 500
# Synthetic, reproducible content: SHA-256 in counter mode over this seed. Same size, same bytes,
# same hashes on every run and every machine.
PAYLOAD_SEED = b"skiff-integration-payload"
SCAN_TIMEOUT_SECONDS = 120
# Every call is bounded, so a stalled RomM fails the seed instead of hanging it.
REQUEST_TIMEOUT_SECONDS = 30
TOKEN_NAME = "skiff-integration"
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
HTTP_OK = 200
HTTP_CREATED = 201
HTTP_FORBIDDEN = 403


def log(message):
    print(f"seed: {message}", file=sys.stderr, flush=True)


def write_payload(size, name=PAYLOAD_NAME, seed=PAYLOAD_SEED):
    """Create a payload once and return its size and hashes (the ones RomM records)."""
    LIBRARY_DIR.mkdir(parents=True, exist_ok=True)
    path = LIBRARY_DIR / name
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


async def scan(cookie):
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
        await client.emit("scan", {"platform_fs_slugs": [PLATFORM_SLUG], "type": "quick", "apis": []})
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
    for name in (UNSUPPORTED_NAME, LONG_NAME):
        write_payload(SPECIAL_BYTES, name, LIBRARY_SEED + name.encode())
        names.append(name)
    return names


def find_roms(auth, names):
    """The platform's id and the ROM id of each file name, in order; every file in the platform's
    folder must have been scanned."""
    status, platforms = request("GET", "/api/platforms", auth)
    platform = next((p for p in platforms or [] if p["fs_slug"] == PLATFORM_SLUG), None)
    if status != HTTP_OK or platform is None:
        raise SystemExit(f"seed: platform {PLATFORM_SLUG} missing after the scan (HTTP {status})")
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
    on_disk = sorted(path.name for path in LIBRARY_DIR.iterdir() if path.is_file())
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


def create_token(auth):
    status, token = request(
        "POST", "/api/client-tokens", auth, {"name": TOKEN_NAME, "scopes": TOKEN_SCOPES}
    )
    if status != HTTP_CREATED or not token:
        raise SystemExit(f"seed: creating the client token failed with HTTP {status}")
    return token["raw_token"]


def main():
    user = os.environ["SKIFF_ADMIN_USER"]
    password = os.environ["SKIFF_ADMIN_PASSWORD"]
    size = int(os.environ.get("SKIFF_PAYLOAD_BYTES", DEFAULT_PAYLOAD_BYTES))
    library_roms = int(os.environ.get("SKIFF_LIBRARY_ROMS", "0"))
    auth = basic_auth(user, password)

    payload = write_payload(size)
    extra = write_payload(EXTRA_BYTES, EXTRA_NAME, EXTRA_SEED)
    library = write_library(library_roms) if library_roms > 0 else []
    create_admin(user, password, auth)
    started = time.monotonic()
    asyncio.run(scan(session_cookie(auth)))
    platform_id, (rom_id, extra_rom_id) = find_roms(auth, [PAYLOAD_NAME, EXTRA_NAME])
    log(f"roms {rom_id}, {extra_rom_id} on platform {platform_id} after {time.monotonic() - started:.1f} s")
    if library:
        log(f"library: {len(library)} more files, and every other file in {LIBRARY_DIR}")
    extra["rom_id"] = extra_rom_id
    print(
        json.dumps(
            {
                "platform_id": platform_id,
                "rom_id": rom_id,
                "token": create_token(auth),
                **payload,
                "extra": extra,
            }
        )
    )


if __name__ == "__main__":
    main()
