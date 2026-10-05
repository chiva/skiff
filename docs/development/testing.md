# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ self-test, TLS probe, KIRK and network probes and benchmark (no ARK), UI prototype (headless) | `scripts/dev.sh psp selftest tls-probe kirk-probe net-probe ui-proto bench` |
| Integration | Docker Compose | ✅ server checks, host transport against the server | `scripts/dev.sh romm-up romm-check romm-test romm-down` |
| Hardware | Real PSP over PSPLINK | ❌ manual | see below |

## Unit tests

- Framework: [Unity](https://github.com/ThrowTheSwitch/Unity). One executable per module in
  `tests/unit/test_<module>.c`, registered with `skiff_add_unit_test()` in `tests/CMakeLists.txt`.
- Tests narrate with `TEST_PRINTF` so a failure in CI shows what was being checked.
- Coverage counts portable code only (`src/` minus `src/platform/`).
- Tests that use the network layer link the same curl and Mbed TLS as the EBOOTs, built into the
  host image, with `skiff_add_tls_unit_test()`.

### Test doubles (`tests/support/`)

- **Fake transport** (`fake_transport.h`): a `skiff_transport` that replays the RomM responses in
  `tests/fixtures/romm/` and injects failures: a timeout or lost connection after N body bytes, a
  refusal before any response, a file that changed on the server. It answers `Range`/`If-Range`
  from the recorded body as RomM does (206, 200 on a stale ETag, 416 past the end) and logs every
  request. Layers above `net/` (`romm/`, `jobs/`) test against it, so an API change shows up as a
  failing test rather than a crash on a PSP.
- **Local HTTP server** (`local_http_server.h`): a scripted server on `127.0.0.1` that sends exact
  bytes back (a cut-off body, a stalled connection, garbage to a TLS client), for the curl
  transport's tests without a network.

The fixtures are recorded, not written by hand: `scripts/dev.sh romm-record` starts a fresh test
RomM with a 4 KiB synthetic file, saves each response byte for byte (CRLF headers, de-chunked body;
`Date` and `Set-Cookie` dropped) and stops the server. Review the files before committing: they
must hold synthetic data only, never a token. The hygiene hooks leave `tests/fixtures/` untouched so
the bytes stay exact.

## Emulator tests

`tests/emulator/run_eboot.sh <EBOOT> <NAME>` boots a check EBOOT in PPSSPPHeadless and passes only
when the output contains `SKIFF <NAME> OK`. It runs `skiff_selftest` (`SELFTEST`),
`skiff_tls_probe` (`TLS PROBE`, see [Security probe](#security-probe)), `skiff_kirk_probe`
(`KIRK PROBE NO ARK`, see [KIRK probe](#kirk-probe)), `skiff_net_probe` (`NET PROBE NO ARK`, see
[Network probe](#network-probe)), `skiff_ui_proto` (`UI PROTO HEADLESS`, see
[UI prototype](#ui-prototype)) and `skiff_bench` (`BENCH NO ARK`, see [Benchmark](#benchmark)). PPSSPPHeadless only shows a
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
- Caddy in front, on four ports: `8080` plain HTTP (for comparison only), `8443` TLS, `8444` TLS
  that requires a client certificate signed by the test CA, and `8445` TLS 1.2 with AES-128-GCM
  only (a server without ChaCha20, for the transport's fallback; not published on the LAN).
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
| `scripts/dev.sh romm-test` | Runs the host build's transport against it ([below](#integration-tests)); ends with `SKIFF TRANSPORT INTEGRATION OK` |
| `scripts/dev.sh romm-record` | Records the fake transport's fixtures from a fresh server, then stops it |
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

### Integration tests

`romm-test` (CI runs it after `romm-check`) builds `tests/integration/test_transport_romm.c` on the
host and runs it on the compose network. It checks the error codes a player would see, over the TLS
stack the PSP uses:

- HTTPS through the test CA works, and without the CA the server is untrusted (105);
- with the clock set to 2000 (a PSP whose battery ran flat) the failure is the clock (108); with
  the clock in 2100 it is an untrusted certificate (105);
- both client certificates pass the mTLS port; no certificate, or one from the untrusted CA, gives
  106;
- plain HTTP works; an unknown host gives 101 and a closed port 102;
- a second request reuses the connection (no new handshake);
- the token is required (401 maps to 200), the whole file downloads, a `Range` with the current
  ETag resumes with exactly the missing bytes, and a stale ETag restarts with the whole file.

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

The **Skiff KIRK probe**, **Skiff network probe**, **Skiff benchmark** and **Skiff UI prototype**
are installed too; run them only when working on entropy (see [KIRK probe](#kirk-probe)),
networking (see [Network probe](#network-probe) and [Benchmark](#benchmark)) or the UI (see
[UI prototype](#ui-prototype)).

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
- **Plain HTTP:** one request, for comparison, when the server publishes it on the LAN
  (`SKIFF_LAN_PLAIN_HTTP=1 scripts/dev.sh romm-lan`); skipped otherwise;
- **Skiff's transport** (`skiff_net`, the layer the app uses): two heartbeats on one kept
  connection, the seeded file downloaded whole (with its speed, checked against the seeded CRC32), resumed from byte 1000 with the
  current ETag (206, the same bytes as the whole download) and with a stale one (the whole file
  again), and a request with the TLS clock forced back to 2000, which must fail with
  `SKIFF_ERR_NET_TLS_CLOCK` (108) before the real clock is put back;
- **Memory:** free system memory and heap use before and after the network modules load, after
  joining, at their worst while the requests run, and after unloading.

On a PSP:

1. Make sure a Network Settings profile can join your access point (Settings → Network Settings,
   [Wi-Fi guide](../guide/02-wifi.md)), and note its position in the list (the first is 1).
2. `scripts/dev.sh romm-lan` on the computer, so the PSP can reach the server.
3. PSP in USB mode: `scripts/memstick.sh install <mount>`. With a test server running, it writes
   `net-probe.ini` (the server's address, the profile (`SKIFF_NET_PROFILE`, default 1), and the
   seeded file, its CRC32 and the API token from `build/integration/romm.json`) and copies the test CA and client
   certificates next to the probe. The token only opens that throwaway server; uninstall removes it
   with the folder. Eject.
4. With the Wi-Fi switch on, run **Skiff network probe**; it returns to the XMB when done (under a
   minute).
5. USB mode: `scripts/memstick.sh results <mount>` → `SKIFF NET PROBE OK`. Each run also appends
   one line to `net-log.txt` (join time, TLS version and cipher, the median handshake times, the
   transport's download time, the lowest free system memory and the highest heap use).

The probe loads the network modules at 333 MHz, as the app does, and fails if the clock did not
change. A failed join or unload names the firmware call and its result. `net_psp` refuses to unload the
network modules while the access point is connected, and stops at the first layer that fails to
come down. Without ARK, as in PPSSPP, TLS cannot start: the
probe loads and unloads the network modules, checks that libcurl refuses to start, and ends with
`SKIFF NET PROBE NO ARK OK` (`scripts/dev.sh net-probe`, run in CI).

## Benchmark

`tests/hardware/bench.c` measures where a download's time goes on a PSP, against the
[integration server](#integration-server), and the settings the app can change. Its sections, in
order (each can be chosen with `sections=` in `bench.ini`):

- **latency:** right after joining Wi-Fi, five back-to-back TCP connects and one after each of 2, 5
  and 10 s of waiting, then heartbeats on one kept HTTPS connection after 0, 0.5, 2, 5 and 15 s of
  idle. A connect near 3 s (marked `SYN retried?`) points at a lost first packet, retried after the
  TCP stack's 3 s timeout; latency that grows with the idle gap points at Wi-Fi power save;
- **cpu:** CRC-32 (bitwise, table, zlib), MD5 and SHA-1 (the hashes RomM records per file), and
  ChaCha20-Poly1305 and AES-128-GCM decryption in 16 KB records (the TLS ciphers), in KB/s and
  cycles per byte. It also times the per-byte digest the network probe ran while it measured its
  download;
- **net:** the seeded file downloaded `runs=` times (default 3, median, min and max) for each of:
  curl buffer 16 to 512 KB; plain HTTP at 16 and 512 KB (only when the server publishes it on the
  LAN); `SO_RCVBUF` 32 and 64 KB (128 KB broke the network stack on a PSP-1000); TLS 1.3 and TLS 1.2, each forcing AES-128-GCM and
  ChaCha20-Poly1305. Speed counts from the first chunk of the body to the
  last (no TCP or TLS setup, no wait for the first byte). A thread at the lowest priority counts
  while the CPU is idle, so each download also reports how busy the CPU was over the same window: if TLS is the limit, HTTPS runs near 100% and gets
  faster at 333 MHz while plain HTTP does not. Every download is checked against the seeded size
  and CRC-32;
- **clock:** HTTPS and plain HTTP at 333 MHz. On a PSP-1000 the clock does not change while Wi-Fi
  is on (the call succeeds, the clock stays at 222 MHz), so set it with `clock_mhz=333`
  (`SKIFF_BENCH_CLOCK_MHZ`), which the network layer applies before its modules load, as in the
  app; if it is not 333 MHz the run fails rather than report 222 MHz results as 333. Without
  `clock_mhz` the benchmark keeps the clock it started with (222 MHz from the XMB), and a probe
  before joining reports whether the clock can change at all;
- **ms:** Memory Stick write and read-back speed with 16 to 512 KB blocks (16 MiB each, compared
  byte for byte, then deleted), then the download written to the Memory Stick in 128 KB blocks as
  it arrives, with the best curl buffer from **net**;
- **app:** the download through Skiff's own transport with its defaults, as the network probe
  measures it.

On a PSP (plugged in, so the battery does not change the run; a full run takes about 25 minutes):

1. On the computer:
   `SKIFF_LAN_PLAIN_HTTP=1 SKIFF_PAYLOAD_BYTES=4194304 scripts/dev.sh romm-lan` (a 4 MiB file, and
   plain HTTP for the comparison).
2. PSP in USB mode: `scripts/memstick.sh install <mount>`. It writes `bench.ini` (the network
   probe's settings, plus `runs=` and `sections=` from `SKIFF_BENCH_RUNS` and
   `SKIFF_BENCH_SECTIONS` when set) and copies the test CA. Eject.
3. With the Wi-Fi switch on, run **Skiff benchmark**. It prints an estimate before each section and
   keeps the PSP from sleeping; HOME → Quit stops it after the current download, and the run then
   ends with `SKIFF BENCH FAIL` (incomplete).
4. To tell power save from lost packets, change Settings → Power Save Settings → WLAN Power Save
   and run the **latency** and **net** sections again
   (`SKIFF_BENCH_SECTIONS=latency,net scripts/memstick.sh install <mount>`).
5. USB mode: `scripts/memstick.sh results <mount>` → `SKIFF BENCH OK`. Each measurement also
   appends one line to `bench-log.txt`, starting with what can change a result: the clock, the power
   source, the WLAN Power Save setting and the state the access point reports, signal strength and
   channel.

A download that fails, or bytes that do not match the seeded file, fail the run; a socket buffer
size the PSP refuses, cuts down or does not report back is reported and skipped. Without ARK, as in PPSSPP, TLS cannot start: the
benchmark loads and unloads the network modules, checks that libcurl refuses to start, runs the
CRC-32 and Memory Stick code on small sizes and ends with `SKIFF BENCH NO ARK OK`
(`scripts/dev.sh bench`, run in CI).

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
