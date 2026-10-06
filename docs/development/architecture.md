# Architecture

## Goals and constraints

- Runs on a PSP-1000: 333 MHz MIPS (Allegrex), 32 MB RAM, 802.11b Wi-Fi, FAT32 Memory Stick.
  Later models have 64 MB; Skiff sets `MEMSIZE=1` in PARAM.SFO to use it (PPSSPP reports a 57 MB
  heap with it).
- Talks to RomM 5.3+ over its REST API, with HTTPS and optional client certificates.
- Usable by people who are not technical: every failure becomes a sentence and a code, in the
  PSP's system language.
- Extensible to other platforms (PS1, emulated systems) without touching the core.
- A download can take an hour (2–4 Mbit/s real Wi-Fi throughput), so everything is designed around
  long, interruptible transfers.

## Layers

```text
  ┌──────────────────────────────────────────────────────────────┐
  │ app/        screens, state machine, input (UI thread)        │
  ├──────────────────────────┬───────────────────────────────────┤
  │ ui/  GU + intraFont      │ installers/  platform plugins     │
  ├──────────────────────────┴───────────────────────────────────┤
  │ jobs/    download/sync worker thread, job queue, persistence │
  │ romm/    typed RomM API client      saves/  SAVEDATA archives│
  ├──────────────────────────────────────────────────────────────┤
  │ net/     transport interface, TLS config, entropy source     │
  │ storage/ logical roots, atomic writes, free space, manifest  │
  │ config/  config.ini, schema migrations                       │
  │ i18n/    message catalogues        log/  redacted log file   │
  ├──────────────────────────────────────────────────────────────┤
  │ core/    errors, version, self-test                          │
  ├──────────────────────────────────────────────────────────────┤
  │ platform/psp  sce*, ARK calls   │ platform/host  TLS hooks    │
  └──────────────────────────────────────────────────────────────┘
```

Only `src/platform/psp/` includes PSP SDK headers. Every other layer compiles on the host, which is
what makes it unit-testable and lets sanitizers run over it.

Status: `core/`, `net/` (transport, TLS entropy source), `storage/` (the storage seam), `jobs/`
(resumable downloads) and `platform/psp/` (lifecycle, network stack, TLS hooks) exist. The other layers arrive with the [roadmap](roadmap.md) phases that need
them.

## Threads, power and suspend

- **UI thread**: input, drawing, system dialogs. Never blocks on the network or the Memory Stick.
- **Worker thread**: runs one job at a time (download, save sync) from a persistent queue, and posts
  progress and results to the UI through a message queue. Modules hold no global mutable state, so
  a job owns its transport and file handles.
- The PSP kernel schedules by priority without time-slicing equal priorities, so the worker runs
  at a lower priority than the UI and yields between chunks.
- **Power**: during a job, `skiff_psp_keep_awake()` (`scePowerTick(PSP_POWER_TICK_SUSPEND)`, at
  most every 5 s) keeps the PSP from auto-sleeping, and the backlight can still dim to save
  battery. A power callback on the same thread as the HOME-menu callback counts suspends and
  resumes (`src/platform/psp/lifecycle.c`): on resume the Wi-Fi connection is gone, so the job
  reconnects and resumes from the `.part` file instead of failing.
- The Wi-Fi switch and the HOME menu are ordinary interruptions with the same resume path. A
  download's stop hook asks the network layer whether the connection still stands
  (`skiff_psp_net_online()`: switch, access point) and whether a suspend happened, so an attempt
  ends soon after instead of at the 30 s stall timeout. Recovery waits for the switch, rejoins the
  profile (or reloads the network modules if that fails) and starts a new transport.
- Measured on a PSP-1000 (`tests/hardware/resume_probe.c`, 64 MiB over TLS 1.3): the Wi-Fi switch
  was noticed 1.7 s after the last byte and the profile rejoined in 13 s; after a suspend the stop
  hook fired on its first poll, rejoining the profile was enough (8 s, no module reload) and the CPU
  was still at 333 MHz; the HOME menu does not pause a download; keep-awake held off Auto Sleep
  through a 270 s download.
- **A suspend invalidates files left open on the Memory Stick**: writes after waking fail (the
  probe's `result.txt` went silent). Every download attempt opens and closes its own files, and the
  check EBOOTs' report reopens its file when a write fails.
- **Clock**: TLS is CPU-bound on a PSP (the CPU is 80–85% busy during an HTTPS download), so the
  network layer runs the CPU at 333 MHz while the network is up and puts the previous clock back
  when it unloads (`skiff_psp_net_load()`, `SKIFF_PSP_NET_CPU_MHZ`). On a PSP-1000 that took HTTPS
  downloads from about 350 to 470 KB/s. The clock has to change before the network modules load:
  with Wi-Fi on, the firmware accepts the call and keeps 222 MHz.

## UI: GU + intraFont

The UI draws with the PSP's GU directly and renders text with intraFont, rather than SDL2:

- Skiff needs the system dialogs, the network picker (`sceUtilityNetconf`) and the on-screen
  keyboard (`sceUtilityOsk`), which must run inside a GU render loop. Mixing them with SDL2's
  renderer is fragile.
- intraFont draws with the fonts in the PSP firmware, so nothing is bundled. Skiff loads only the
  Latin font (`ltn0.pgf`), which covers the accented letters English and Spanish need; a character
  it lacks is drawn as a placeholder.
- Japanese is not supported at first: intraFont loads a font file whole into the heap, and the
  Japanese one costs megabytes on a 32 MB PSP. If players ask for it, `jpn0.pgf` becomes the Latin
  font's fallback (`intraFontSetAltFont()`), loaded only when a string needs it. Text is UTF-8
  throughout, so that change stays inside `ui/`.
- `libintrafont` is packaged by pspdev. Its licence (CC BY-SA 3.0) ships with the release's
  third-party licences once the app links it (`scripts/psp-packages.txt`).

Phase 1 confirms this with a prototype on hardware before the UI layer is written
(`tests/prototype/ui_proto.c`, see [Testing](testing.md#ui-prototype)). The prototype still draws a
Japanese line and measures both fonts, so the cost of adding Japanese later is known.

## Errors and languages

`include/skiff/error.h` holds one X-macro table that generates the `skiff_err` enum, the stable
names and the English messages. Codes are grouped by layer (1xx network, 2xx RomM, 3xx storage,
4xx config) and never renumbered, because players quote them in bug reports. The
[troubleshooting guide](../guide/troubleshooting.md) documents every code, and a pre-commit check
(`scripts/check-error-docs.sh`) fails if a code is missing from it.

When the UI arrives, the English messages become the fallback catalogue in `i18n/`. Other
languages (Spanish first) are catalogues keyed by the same stable names, chosen from the PSP's
system language setting. Logs always use the stable name and number, never translated text.

## Logging

`log/` writes `PSP/GAME/Skiff/skiff.log`: levels, a size cap with one rotated file, and batched
writes, because small writes to a Memory Stick are slow. Secrets are redacted at the logging call
(tokens, keys, `Authorization` headers), not left to callers. The bug report form asks for this file.

## RomM integration

| Need | RomM API |
|---|---|
| Version check | `GET /api/heartbeat` |
| Pairing | Device-code flow: `POST /api/auth/device/init`, the user approves in the web UI, the PSP polls for the token |
| Fallback auth | Client API token (`rmm_…`) as `Authorization: Bearer` |
| Browse | Platforms and ROMs endpoints, **paginated** so one JSON page stays small |
| Download | `GET /api/roms/{id}/content/{file_name}` with `Range` and `If-Range` |
| Saves | `POST /api/saves`, `GET /api/saves/{id}/content`, `POST /api/sync/negotiate`, tagged with this PSP's `device_id` |

Custom headers: `config.ini` may define extra HTTP headers sent on every request. This covers
proxy access schemes the PSP cannot otherwise satisfy — notably Cloudflare Access service tokens
(`CF-Access-Client-Id` / `CF-Access-Client-Secret`) — without a VPN or a client certificate. It is
a few lines in the transport and a cheaper alternative to mTLS for users already behind such a
proxy. Header values are secrets: redacted in logs, same caveats as the token.

Version policy: below the minimum supported RomM version, Skiff refuses to run
(`SKIFF_ERR_ROMM_UNSUPPORTED_VERSION`). Newer versions are allowed with a one-time notice, because
refusing every untested release would break Skiff with each RomM update.

JSON is parsed one page at a time with cJSON (packaged by pspdev); responses are never held whole.
ROM lists are requested with `with_char_index`, `with_filter_values` and `with_rom_id_index` set
to false: RomM includes those by default, and they grow with the whole library, not the page.
RomM records CRC32, MD5 and SHA-1 for every file (`files[]` in a ROM), so the integrity check can
use any of them.

## Downloads and storage

`include/skiff/download.h` runs one download attempt; the caller (`jobs/`) decides whether to try
again (`skiff_download_retryable()`: Wi-Fi, name lookup, connect, timeout, lost connection) once the
network is back.

- **Atomic**: download to `<target>.part`, then rename into place. A pulled Memory Stick or a
  power-off never leaves a half-written file under the final name.
- **Progress**: `<target>.resume` records how many bytes of the `.part` file are on the device, the
  CRC-32 of exactly those bytes, the ETag and the file's expected size and CRC-32. Every 4 MiB
  (`SKIFF_DOWNLOAD_CHECKPOINT_BYTES`, about 9 s at 470 KB/s) the buffered bytes are written and
  synced first, then the `.resume` file is rewritten and synced, so it never vouches for bytes the
  Memory Stick does not hold. An attempt that fails for any reason but the Memory Stick saves its
  exact offset before returning. FAT has no atomic replace, so the file ends with a CRC-32 of its
  own lines: one cut short by a power loss is refused and the download starts over. A `.resume` file
  for another size or CRC-32, or a `.part` file shorter than its offset, also means starting over.
- **Resume**: `Range` from the saved offset plus `If-Range` with the saved ETag, so a file that
  changed on the server comes back whole (200) and restarts instead of being stitched from two
  versions. Only a strong ETag is kept: a weak one (`W/`) cannot be used with `If-Range`, so such a
  download always restarts. A server that ignores ranges also answers 200, with the same result.
- **Checks before writing**: the status and headers are judged before the first byte touches the
  Memory Stick. Only a 200 with the expected size, or a 206 starting at the saved offset of a file of
  the expected size, is written; an error page (a proxy's login page, a 401) leaves the `.part` and
  `.resume` files as they were. A 416 means the server no longer has what was being resumed: the
  progress is dropped.
- **Integrity**: the CRC-32 RomM records for the file is computed while downloading (zlib, 33 MB/s
  on a PSP-1000), continued from the saved value on resume, so resuming never re-reads the `.part`
  file. A finished file that does not match is deleted with its progress (206): the usual cause is a
  file replaced on the server without a rescan, so RomM's checksum is stale.
- **Throughput**: writes go to the Memory Stick in 128 KB blocks
  (`SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES`); it is slow with the small chunks the network delivers.
- **Storage seam**: the download writes through `include/skiff/storage.h`, not the C library. On the
  PSP newlib's `off_t` is 32 bits, so stdio cannot place a file position past 2 GiB; the PSP
  implementation uses `sceIo` with 64-bit offsets. A rename never replaces a file (FAT cannot do it
  in one step): the target is removed first.
- **Limits**: check free space before starting; refuse files over 4 GB (FAT32, 304).
- **Names**: names from RomM are sanitised: no path separators, no `..`, FAT-safe characters only.
- **Installed state**: a manifest (`PSP/GAME/Skiff/installed.json`: RomM ID → path, size, hash)
  records what Skiff installed. Scanning folders and matching names is only a fallback for games
  copied by hand.
- **Logical roots**: code addresses `games:`, `saves:`, `app:`; `storage/` maps them to `ms0:` or
  `ef0:` (PSP Go) on hardware, and to a temporary directory in host tests.

## Installers (the platform plugin seam)

An installer maps a RomM platform to where its games go:

```c
typedef struct skiff_installer {
    const char *romm_platform_slug;     /* "psp" */
    const char *const *extensions;      /* {".iso", ".cso", ".zso", NULL} */
    const char *target_dir;             /* "games:/ISO" */
    skiff_err (*post_install)(const char *installed_path); /* optional */
} skiff_installer;
```

The first release registers one installer (PSP). PS1 and emulated systems are new table entries;
see [Adding a platform](adding-a-platform.md).

## Saves

PSP saves are folders under `PSP/SAVEDATA/` named after the game ID (e.g. `ULUS10064DATA00`).
They are encrypted per game, not per console, so they move between PSPs and PPSSPP. RomM stores
saves as single files. Skiff:

1. Gets the game ID from the ISO using [argosy-sigil](https://github.com/rommapp/argosy-sigil),
   the library RomM's own clients use. Its memory use and dependencies on the PSP are checked
   before adoption.
2. Zips every `SAVEDATA` folder for that ID in the layout RomM's in-browser PPSSPP player expects,
   and uploads it with `emulator=ppsspp`, so saves move between the PSP, PPSSPP and RomM's web
   player.
3. Uses `/api/sync/negotiate` and never overwrites silently: when both sides changed, the player
   chooses.

A game and Skiff never run at the same time, so a save is never synced while it is being written.

## Testing seams

- `net/` exposes a `transport` interface (`include/skiff/transport.h`). Host tests use a fake
  transport that replays recorded RomM responses and injects failures: timeouts, truncated bodies,
  a changed ETag (`tests/support/fake_transport.h`). The real curl transport is tested against a
  scripted local server and, in CI, against the integration RomM; see [Testing](testing.md).
- `storage/` exposes a `storage` interface (`include/skiff/storage.h`). Host tests use a POSIX
  implementation over a temporary directory (`src/platform/host/storage_posix.h`), wrapped by a
  fake that injects what a Memory Stick does to a long download: it fills up mid-write, a sync or
  rename fails, open file handles stop working as after a suspend (`tests/support/fake_storage.h`).
  `storage/` roots will point at a temporary directory on the host.
- The self-test checks what differs between host, emulator and hardware (C library, heap, clock,
  byte order) and grows with each layer. See [Testing](testing.md).

## Security design

### Transport

The PSP's built-in HTTPS stops at TLS 1.0 and cannot present client certificates, so Skiff
brings its own TLS stack: libcurl over mbedtls, behind the `transport` interface. Why it differs
from pspdev's packages is explained in [TLS on the PSP](tls.md).

pspdev's packages are too old to ship: curl 7.64.1 (2019) and mbedtls 2.28, out of support since
the end of 2024 and TLS 1.2 only. Skiff's toolchain image (`docker/toolchain.Dockerfile`, see
[Toolchain](toolchain.md#the-skiff-toolchain-image)) removes them and builds curl 8.22 and
Mbed TLS 4.1 LTS (TLS 1.3, supported until March 2029) from checksum-pinned archives. Mbed TLS 3.6
LTS was the original plan, but its support ends in March 2027, before Skiff's first networking
release would have had a meaningful life. The image is built from source in every build rather
than pulled from a registry, so there is no mutable published artifact to trust. wolfSSL is
excluded because its GPL-2.0 licence is incompatible with Skiff's MIT licence.

The `transport` interface (`include/skiff/transport.h`) takes a URL, headers, `Range`/`If-Range`
and a body callback, and fills a small response record: status, ETag, Content-Length and
Content-Range, read by one header parser shared with the fake transport, plus the TLS version and
cipher suite of the connection, for logs. Custom headers from
`config.ini` are sent on every request and may not contain line breaks. Only URLs with an
explicit `http://` or `https://` are sent: curl would guess plain HTTP for a bare address and send
the token in the clear, so a server address without a scheme is a configuration error (402). The curl transport
(`include/skiff/curl_transport.h`) keeps one handle, and so one connection, per session. It sets no
limit on a whole transfer, since a download can take an hour; a connection that delivers nothing for
30 seconds (`SKIFF_CURL_STALL_TIMEOUT_S`) counts as a timeout instead. A request can also carry a
stop hook, asked about once a second even while no bytes arrive, so the caller can end a transfer as
soon as it knows the network is gone (Wi-Fi switch off, suspend) or the player cancels, instead of
waiting out the stall timeout.

Every failure becomes one code (`skiff_net_error_from_curl()`, table-tested in
`tests/unit/test_net_errors.c` and checked against Caddy by the integration tests):

| What happened | Code |
|---|---|
| Name not found / connection refused / connect or stall timeout | 101 / 102 / 103 |
| TLS handshake failed | 104, or 106 with a client certificate configured |
| Server certificate rejected, clock plausible / clock before `SKIFF_TLS_CLOCK_FLOOR` | 105 / 108 |
| Server closed the connection right after the handshake (how a TLS 1.3 server refuses a missing or unaccepted client certificate); client certificate unreadable | 106 |
| Connection broke after the response started, without TLS, or on a connection kept from an earlier request | 111 |
| The body callback or the stop hook stopped the transfer (e.g. Memory Stick full, Wi-Fi switch off) | the callback's code |
| Bad server address or unreadable CA file | 402 |

An HTTP response is not a network failure: callers map its status with `skiff_http_status_error()`
(401, 403, 404 → 200–202; 408 and 504 → 103; 502 → 102; other 5xx → 203).

Skiff sets no cipher list. Mbed TLS's default order offers ChaCha20-Poly1305 first for both TLS
1.3 and 1.2, and keeps AES-GCM and the rest for servers without it. On a PSP-1000 ChaCha20 decrypts
eight times faster than AES-128-GCM (3 MB/s against 0.37 MB/s), and with AES-GCM HTTPS downloads
drop from about 350 to 175 KB/s. Servers that honour the client's order pick ChaCha20, and so do
Go's (Caddy, Traefik), which read a client listing ChaCha20 first as one without AES hardware.
`tests/unit/test_host_tls.c` pins that order, so an Mbed TLS update cannot change it silently. The
integration tests check that the transport negotiates ChaCha20 with Caddy and still reaches a TLS
1.2 server that offers only AES-128-GCM.

For mTLS, client keys should be ECDSA P-256: on a PSP-1000, a TLS 1.3 handshake took 0.59 s
without a client certificate, 0.69 s with an ECDSA P-256 one and 2.07 s with RSA-2048 (median of
5). Connections are kept alive to pay the handshake once per session; a second request on a kept
connection needs none.

### Randomness for TLS

TLS is only as strong as its random numbers: they make the ECDHE keys and session keys. Mbed TLS's
built-in entropy sources only support Unix and Windows, so on the PSP the application must supply
one. The toolchain image compiles the built-in source out (`MBEDTLS_PSA_BUILTIN_GET_ENTROPY`) and
enables `MBEDTLS_PSA_DRIVER_GET_ENTROPY`: every random byte TLS uses, including inside libcurl, is
seeded from one function Skiff supplies, `mbedtls_platform_get_entropy()`. An EBOOT that links
mbedtls without it fails to link, so TLS cannot end up seeded from anything else.
`tests/security/tls_probe.c` verifies the routing on every PR.

Two constraints from Mbed TLS shape Skiff's source:

- **Full entropy per call.** TF-PSA-Crypto 1.x only accepts output credited at 8 bits per byte; less
  counts as failure. Skiff returns `PSA_ERROR_INSUFFICIENT_ENTROPY` rather than over-claiming.
- **Ready at startup.** libcurl calls `psa_crypto_init()` inside `curl_global_init()`, which seeds
  the DRBG there and then. The source must be able to answer before the user has touched anything.

The source is the KIRK crypto engine's hardware random generator. Only kernel mode can reach it, but
ARK-4 and ARK-5 export `sctrlKernelRand()` to applications (it runs KIRK's random command in kernel
mode), so Skiff needs no kernel module of its own. `src/platform/psp/kirk_entropy.c` passes KIRK's
32-bit words to Mbed TLS unmodified (Mbed TLS's random generator conditions its seed itself), behind
the health tests in `src/net/entropy.c`: SP 800-90B's repetition count and adaptive proportion
tests, which turn TLS off for the session if the generator repeats a word, alternates or cycles.

There is no fallback. Without ARK, or after a health failure, the hook refuses and TLS
initialisation fails. `skiff_psp_entropy_status()` tells the two apart, so the player sees what to
do: `SKIFF_ERR_NET_NEEDS_ARK` (install ARK) or `SKIFF_ERR_NET_ENTROPY` (generator failure, report a
bug). The KIRK probe checks the no-ARK path in CI, where PPSSPP has no ARK. Skiff does not fall back
to the toolchain's default source, which derives its output from the clock
([TLS](tls.md#what-pspdev-provides)). The refusal covers plain HTTP too: since curl 7.57,
`curl_global_init()` always initialises TLS, so without entropy no connection of any kind can be
made.

KIRK alone is enough because its sequence does not restart: on a PSP-1000 with ARK-5 the
[KIRK probe](testing.md#kirk-probe) saw different output on each of 8 launches, power cycles
included. A 128-byte request (what Mbed TLS asks for) takes about 5 ms, and `psa_crypto_init()`
about 11 ms, once per session. The system timer, by contrast, restarts at every launch (each run
started at 5.92–5.95 s), so clock-derived seeds would repeat; nothing is mixed in from it.

### The PSP's clock

Certificate validation needs the right date, and the PSP's clock resets when the battery goes
completely flat. When the server's certificate is rejected while the clock reads earlier than
`SKIFF_TLS_CLOCK_FLOOR` (2026-10-01, before this code existed, so the clock cannot be right), Skiff
reports `SKIFF_ERR_NET_TLS_CLOCK` ("set the date") rather than an untrusted certificate: the date
has to be fixed before anything else about the certificate can be judged. The check reads the same
clock Mbed TLS verified against (`mbedtls_time()`). It cannot look at the reason for the rejection,
because curl's Mbed TLS backend does not report the verify flags. A clock set in the future is not
detected; it shows as an untrusted certificate. Skiff never takes the time from the server it is
trying to verify.

### Secrets on the Memory Stick

The token and the mTLS key sit in plain files on removable storage; the PSP has no secure storage.
Mitigations: one revocable token and certificate per device, and docs that say so plainly.

## Licensing of the binary

Skiff's code is MIT. The EBOOT also contains code from the toolchain and every linked package, and
the release zip ships all their licences in `third-party-licenses/`, collected by pspdev's
`psp-create-license-directory` (see `scripts/psp-packages.txt`). Notable cases:

- **pthread-embedded (LGPL-2.1+)** is linked into every pspdev program through newlib's locking.
  LGPL §6 lets players relink with a modified copy; Skiff meets this by publishing its complete
  source and build under MIT with a public toolchain.
- **mbedtls (Apache-2.0)** requires its licence in the distribution.
- **argosy-sigil (MPL-2.0)**, once adopted, requires its source to stay available, which the
  pinned public submodule does.
