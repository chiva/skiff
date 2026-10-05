# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ self-test, TLS probe, KIRK and network probes (no ARK), UI prototype (headless) | `scripts/dev.sh psp selftest tls-probe kirk-probe net-probe ui-proto` |
| Integration server | Docker Compose | ✅ server checks | `scripts/dev.sh romm-up romm-check romm-down` |
| Hardware | Real PSP over PSPLINK | ❌ manual | see below |

## Unit tests

- Framework: [Unity](https://github.com/ThrowTheSwitch/Unity). One executable per module in
  `tests/unit/test_<module>.c`, registered with `skiff_add_unit_test()` in `tests/CMakeLists.txt`.
- Tests narrate with `TEST_PRINTF` so a failure in CI shows what was being checked.
- Coverage counts portable code only (`src/` minus `src/platform/`).

Planned additions:

- **Contract tests**: recorded RomM 5.3 API responses in `tests/fixtures/romm/`, parsed by the real
  client code, so an API change shows up as a failing test rather than a crash on a PSP.
- **Integration tests**: the host build of the client against the
  [integration server](#integration-server).

## Emulator tests

`tests/emulator/run_eboot.sh <EBOOT> <NAME>` boots a check EBOOT in PPSSPPHeadless and passes only
when the output contains `SKIFF <NAME> OK`. It runs `skiff_selftest` (`SELFTEST`),
`skiff_tls_probe` (`TLS PROBE`, see [Security probe](#security-probe)), `skiff_kirk_probe`
(`KIRK PROBE NO ARK`, see [KIRK probe](#kirk-probe)), `skiff_net_probe` (`NET PROBE NO ARK`, see
[Network probe](#network-probe)) and `skiff_ui_proto` (`UI PROTO HEADLESS`, see
[UI prototype](#ui-prototype)). PPSSPPHeadless only shows a
program's stdout inside its full log (`-l`, lines starting `I stdout: `), so the script extracts
those lines and prints the end of the log when the marker is missing. The first local run builds the
PPSSPP image, which takes several minutes; later runs reuse it.

PPSSPP does not emulate the PSP's Wi-Fi hardware or real Memory Stick timing, so networking and
storage behaviour must also be checked on hardware. Its timings mean nothing either: frame and load
times count only from a PSP.

`docker/ppsspp.Dockerfile` patches one line of PPSSPP: on Linux it refuses every program read of
`flash0:` (where the firmware fonts live). The build fails if the patch stops applying, so a PPSSPP
bump shows whether it is still needed.

## Integration server

`tests/integration/` runs a disposable RomM behind a TLS proxy, for the integration tests and for
the PSP's network checks:

- RomM 5.3.1 and MariaDB, with nothing published but the proxy. Every image is pinned by tag and
  digest in `compose.yaml`.
- Caddy in front, on three ports: `8080` plain HTTP (for comparison only), `8443` TLS, and `8444`
  TLS that requires a client certificate signed by the test CA.
- `gen-certs.sh` creates the test CA, the proxy's certificate (`localhost`, `proxy`, `127.0.0.1`,
  plus the LAN address with `romm-lan`) and client certificates, ECDSA P-256 and RSA-2048, plus one
  from an untrusted CA. They are kept while valid, so copies on a PSP keep working; the proxy's
  certificate is reissued when its addresses change.
- `seed.py` runs inside the RomM container. It creates the admin, writes a synthetic 1 MiB file to
  the `psp` platform (the same bytes every run), scans it and creates an API token.

| Command | What it does |
|---|---|
| `scripts/dev.sh romm-up` | Starts a fresh server on `127.0.0.1` (new secrets, empty volumes, about a minute) |
| `scripts/dev.sh romm-lan` | The same with the TLS ports on the LAN, for a PSP; the address is detected, or set `SKIFF_LAN_IP`. Plain HTTP stays local unless `SKIFF_LAN_PLAIN_HTTP=1` |
| `scripts/dev.sh romm-check` | Checks what the client relies on (below); ends with `SKIFF INTEGRATION SERVER OK` |
| `scripts/dev.sh romm-down` | Stops it and deletes its volumes |

There is one test RomM per machine, since the ports are fixed. The commands refuse to touch one
that another checkout (a parallel worktree) started.

`romm-check` (CI runs it on every PR) checks:

- plain HTTP and HTTPS reach RomM, at the version pinned in `compose.yaml`;
- TLS 1.2 and 1.3 both work over HTTP/1.1, and a client that sends no SNI (a PSP connecting by IP
  address) still gets the test certificate;
- a client without the test CA is refused;
- the mTLS port refuses no certificate and the untrusted one, and accepts both test client
  certificates;
- the API refuses requests without the token and accepts it;
- RomM reports the seeded file's size, CRC32, MD5 and SHA-1, and the download matches;
- `Range` with the current ETag in `If-Range` resumes (206), and a stale ETag restarts with the
  whole file (200).

Everything generated lives in `build/integration/` (git-ignored):

- `certs/`: copy `ca.crt` and a client certificate and key to the PSP;
- `romm.env`: the secrets, including the web UI password for the user `skiff`;
- `romm.json`: the seeded file's ID, size and hashes, and the API token.

The keys and the token are test material for a server that only lives on your machine: never
commit them, and do not reuse them anywhere else. The proxy container only gets the server's key
and the CA certificate. With `romm-lan` the server is reachable by anyone on your network until
`romm-down`.

## Hardware tier

Run before merging changes to `src/net/`, `src/storage/`, `src/platform/psp/`, or a toolchain
bump. Record the PSP model and firmware in the PR.

### From the XMB (no PSPLINK, nothing to install)

Every check EBOOT also writes its output to `result.txt` next to its `EBOOT.PBP`, flushed line by
line, so a run started from the XMB can be read back from the Memory Stick.

1. Build: `scripts/dev.sh psp`.
2. Put the PSP in USB mode (or use a card reader), then `scripts/memstick.sh install <mount>`
   (e.g. `/Volumes/PSP`). Eject.
3. On the PSP, from Game → Memory Stick, run **Skiff self-test** and **Skiff TLS probe**. Each
   returns to the XMB when done.
4. Back in USB mode: `scripts/memstick.sh results <mount>` → expect `SKIFF SELFTEST OK` and
   `SKIFF TLS PROBE OK`. A missing or truncated `result.txt` means the EBOOT crashed; its last line
   shows how far it got.
5. Run **Skiff**: check the version on screen and that START exits; then HOME → Quit must exit
   without freezing.
6. `scripts/memstick.sh uninstall <mount>` removes the folders when done (it keeps nothing else).

The **Skiff KIRK probe**, **Skiff network probe** and **Skiff UI prototype** are installed too;
run them only when working on entropy (see [KIRK probe](#kirk-probe)), networking (see
[Network probe](#network-probe)) or the UI (see [UI prototype](#ui-prototype)).

### Over PSPLINK

1. Set up PSPLINK ([debugging](debugging.md)) and build: `scripts/dev.sh psp`.
2. In `pspsh`: `./build/psp/skiff_selftest.prx` → expect `SKIFF SELFTEST OK`, and
   `./build/psp/skiff_tls_probe.prx` → expect `SKIFF TLS PROBE OK`.
3. Run the app: `./build/psp/skiff.prx`, check the version on screen, press START, and
   check it returns cleanly.

From the networking release on, the hardware tier adds: joining the TKIP test SSID; an HTTPS
request to a test RomM; TLS 1.3 (or 1.2 with ECDHE) confirmed in a packet capture; a proxy rejecting a
missing or wrong client certificate; a download interrupted (Wi-Fi switch off) and resumed; and
installing a homebrew ISO that then boots from ARK-5.

## Security probe

`scripts/dev.sh tls-probe` runs `tests/security/tls_probe.c` (CI runs it on every PR). It checks that
libcurl uses Mbed TLS 4.1 with HTTP and HTTPS only, and that TLS randomness comes **only** from
`mbedtls_platform_get_entropy()`: the probe's version of that hook refuses every request, and both
`psa_crypto_init()` and `curl_global_init()` must then fail after calling it. If anything else could
seed TLS, they would succeed and the probe fails.

## KIRK probe

`tests/security/kirk_probe.c` measures the KIRK crypto engine's random generator, read through ARK's
`sctrlKernelRand()`, and Skiff's entropy hook built on it. On a PSP with ARK it reports, per run:

- how long a 128-byte gather (one Mbed TLS entropy request, 32 calls) takes, next to the same gather
  from the C library's `getentropy()` (the toolchain's default source) as a baseline;
- 1024 KIRK values checked for repeats, bit balance and byte distribution (the baseline is timed
  only);
- Skiff's TLS stack seeded by the real hook: `psa_crypto_init()` and `curl_global_init()` must
  succeed (their times are reported), and two `psa_generate_random()` draws must differ;
- last, a request the hook does not support (non-zero flags) must be refused and turn the status to
  `SKIFF_ERR_NET_ENTROPY`.

It ends with `SKIFF KIRK PROBE OK`/`FAIL` (a baseline failure or an unwritten log also fails the run)
and appends a fingerprint line (uptime, the first KIRK values in `first=`, and all timings) to
`kirk-log.txt`, after comparing its `first=` values with every earlier run in that file: a match
fails the run, since it would mean KIRK produced the same sequence twice, e.g. restarting it after
every power-on. Run it several times, power-cycling the PSP in between; a single run has nothing to
compare against.
`scripts/memstick.sh results` prints both files.

Without ARK there is nothing to measure, so the probe checks instead that Skiff's hook refuses, TLS
fails closed, and the reason reported is `SKIFF_ERR_NET_NEEDS_ARK` (`SKIFF KIRK PROBE NO ARK OK`).
PPSSPP has no ARK, so that is what CI runs (`scripts/dev.sh kirk-probe`).

## Network probe

`tests/hardware/net_probe.c` makes the first HTTPS requests from a PSP, against the
[integration server](#integration-server). It uses Skiff's network layer
(`src/platform/psp/net_psp.c`) and its real entropy hook, and checks:

- **Wi-Fi:** it joins the access point of a saved Network Settings profile, without the network
  picker, and reports the time and the IP address;
- **HTTPS:** the heartbeat request is trusted through the test CA, and runs over TLS 1.3, or TLS 1.2
  with an ECDHE key exchange (version and cipher suite are reported). A client that trusts a
  different CA must fail to verify the server's certificate;
- **Client certificates:** the mTLS port refuses a request without one and one from an untrusted CA
  (TCP connects, then TLS ends before any HTTP response). It accepts the ECDSA P-256 and RSA-2048
  test certificates. A missing certificate file fails the run as a setup error, never as a refusal;
- **Handshake times:** the median of 5 fresh connections for HTTPS and for each client certificate
  type;
- **Keep-alive:** a second request on the same connection needs no new handshake;
- **Plain HTTP:** one request, for comparison;
- **Memory:** free system memory and heap use before and after the network modules load, after
  joining, at their worst while the requests run, and after unloading.

On a PSP:

1. Make sure a Network Settings profile can join your access point (Settings → Network Settings,
   [Wi-Fi guide](../guide/02-wifi.md)), and note its position in the list (the first is 1).
2. `scripts/dev.sh romm-lan` on the computer, so the PSP can reach the server.
3. PSP in USB mode: `scripts/memstick.sh install <mount>`. With a test server running, it writes
   `net-probe.ini` (the server's address and the profile, `SKIFF_NET_PROFILE`, default 1) and copies
   the test CA and client certificates next to the probe. Eject.
4. With the Wi-Fi switch on, run **Skiff network probe**; it returns to the XMB when done (under a
   minute).
5. USB mode: `scripts/memstick.sh results <mount>` → `SKIFF NET PROBE OK`. Each run also appends
   one line to `net-log.txt` (join time, TLS version and cipher, the median handshake times, the
   lowest free system memory and the highest heap use).

A failed join or unload names the firmware call and its result. `net_psp` refuses to unload the
network modules while the access point is connected, and stops at the first layer that fails to
come down. Without ARK, as in PPSSPP, TLS cannot start: the
probe loads and unloads the network modules, checks that libcurl refuses to start, and ends with
`SKIFF NET PROBE NO ARK OK` (`scripts/dev.sh net-probe`, run in CI).

## UI prototype

`tests/prototype/ui_proto.c` checks the UI stack chosen in
[Architecture](architecture.md#ui-gu--intrafont) before the UI layer exists: GU draws a 20-item list
with intraFont, using the firmware's Latin font with its Japanese font as fallback, and the
on-screen keyboard and the network picker open from the render loop. It links the app's module info,
so its memory figures are the app's.

On a PSP:

1. Run **Skiff UI prototype** and press a button within 10 seconds (otherwise it exits on its
   own, as described below). Check that the title, the Japanese line and the first row (accented
   Latin: `café, señor`) render, and that Up/Down move the highlight.
2. Triangle opens the keyboard: type at least one character and confirm (cancelling fails the
   run). The text appears top right.
3. Square loads the network modules and opens the network picker: pick a connection that works
   and let it connect (cancelling or a failed connection fails the run). The IP address appears top
   right. When the picker closes, the prototype drops the connection, waits until it is gone, and
   unloads the modules.
4. HOME → Quit (or START) ends the run.

Each step is written to `result.txt` as it happens (dialog opened, shown, closed; any failing
`sceUtility*` call with its code; connection, disconnection and unloading), so a failed run says
where it stopped. The summary at the end has: font load times; heap used by the Latin font alone
and by both; the first
frame read back (drawn pixels in a Latin and a Japanese-only line, both must be above zero); list
frame time (mean, max, frames over the 16.7 ms budget); each dialog's outcome; and system memory
before the network modules load, after they load and after they unload, with how much the
unloading gave back (reported, not judged).

It ends with `SKIFF UI PROTO OK` only if: both fonts rendered; the keyboard was shown, and text
was typed and confirmed; the picker was shown and connected (an IP address was obtained); the
connection was dropped and the modules unloaded; and no dialog call failed or timed out (a dialog
left open for 5 minutes is closed and fails) and no controller read failed. Otherwise it ends with
`SKIFF UI PROTO FAIL`.

With no button pressed for 10 seconds of system time it exits on its own, checking only the fonts
and the frames, and ends with `SKIFF UI PROTO HEADLESS OK`/`FAIL`. That is what CI runs
(`scripts/dev.sh ui-proto`); the 10 seconds stay well inside the emulator script's 30-second limit.
Switches (HOLD, Wi-Fi) and HOME do not count as a button press.

## Test data rules

Homebrew, public domain or synthetic only. No commercial games, BIOS files, real saves, tokens or
keys, ever.
