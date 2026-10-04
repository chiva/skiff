"""Seed the integration RomM: admin user, one synthetic PSP file, a library scan, a client token.

Runs inside the RomM container with RomM's own Python (`scripts/dev.sh romm-up` does this), so it
needs nothing installed elsewhere: the standard library plus the python-socketio client RomM ships.
The scan is started over RomM's socket, the way its web UI does, because RomM has no REST endpoint
for it. Prints one JSON object on stdout for the tests; the token in it is a secret.

Environment: SKIFF_ADMIN_USER, SKIFF_ADMIN_PASSWORD, SKIFF_PAYLOAD_BYTES (optional).
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
# Synthetic, reproducible content: SHA-256 in counter mode over this seed. Same size, same bytes,
# same hashes on every run and every machine.
PAYLOAD_SEED = b"skiff-integration-payload"
SCAN_TIMEOUT_SECONDS = 120
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
HTTP_CREATED = 201
HTTP_FORBIDDEN = 403


def log(message):
    print(f"seed: {message}", file=sys.stderr, flush=True)


def write_payload(size):
    """Create the payload once and return its size and hashes (the ones RomM records)."""
    LIBRARY_DIR.mkdir(parents=True, exist_ok=True)
    path = LIBRARY_DIR / PAYLOAD_NAME
    sha1, md5, crc = hashlib.sha1(), hashlib.md5(), 0
    with path.open("wb") as out:
        written, counter = 0, 0
        while written < size:
            block = hashlib.sha256(PAYLOAD_SEED + counter.to_bytes(8, "big")).digest()
            block = block[: size - written]
            out.write(block)
            sha1.update(block)
            md5.update(block)
            crc = zlib.crc32(block, crc)
            written += len(block)
            counter += 1
    return {
        "file_name": PAYLOAD_NAME,
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
        with urllib.request.urlopen(req) as response:
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
    with urllib.request.urlopen(req) as response:
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

    await client.connect(API, headers={"Cookie": cookie}, socketio_path="/ws/socket.io")
    try:
        await client.emit("scan", {"platform_fs_slugs": [PLATFORM_SLUG], "type": "quick", "apis": []})
        stats = await asyncio.wait_for(outcome, SCAN_TIMEOUT_SECONDS)
        log(f"scan done: {stats}")
    finally:
        await client.disconnect()


def find_rom(auth):
    status, platforms = request("GET", "/api/platforms", auth)
    platform = next((p for p in platforms or [] if p["fs_slug"] == PLATFORM_SLUG), None)
    if status != 200 or platform is None:
        raise SystemExit(f"seed: platform {PLATFORM_SLUG} missing after the scan (HTTP {status})")
    status, page = request("GET", f"/api/roms?platform_ids={platform['id']}", auth)
    items = (page or {}).get("items", [])
    rom = next((r for r in items if r["fs_name"] == PAYLOAD_NAME), None)
    if status != 200 or rom is None:
        raise SystemExit(f"seed: {PAYLOAD_NAME} missing after the scan (HTTP {status})")
    return platform["id"], rom["id"]


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
    auth = basic_auth(user, password)

    payload = write_payload(size)
    create_admin(user, password, auth)
    started = time.monotonic()
    asyncio.run(scan(session_cookie(auth)))
    platform_id, rom_id = find_rom(auth)
    log(f"rom {rom_id} on platform {platform_id} after {time.monotonic() - started:.1f} s")
    print(
        json.dumps(
            {"platform_id": platform_id, "rom_id": rom_id, "token": create_token(auth), **payload}
        )
    )


if __name__ == "__main__":
    main()
