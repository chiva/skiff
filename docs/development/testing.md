# Testing

Skiff is tested at three levels. The same self-test checks (`src/core/selftest.c`) run in all
three, so a failure on hardware that does not happen on the host points at the PSP environment, not
the logic.

| Tier | Where | In CI | Command |
|---|---|---|---|
| Unit | Host (Linux/macOS) | ✅ gcc + clang, plain + ASan/UBSan, coverage ≥85% | `scripts/dev.sh test asan coverage` |
| Emulator | PPSSPPHeadless | ✅ self-test, TLS probe, KIRK probe (no ARK) | `scripts/dev.sh psp selftest tls-probe kirk-probe` |
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
`skiff_tls_probe` (`TLS PROBE`, see [Security probe](#security-probe)) and `skiff_kirk_probe`
(`KIRK PROBE NO ARK`, see [KIRK probe](#kirk-probe)). PPSSPPHeadless only shows a
program's stdout inside its full log (`-l`, lines starting `I stdout: `), so the script extracts
those lines and prints the end of the log when the marker is missing. The first local run builds the
PPSSPP image, which takes several minutes; later runs reuse it.

PPSSPP does not emulate the PSP's Wi-Fi hardware or real Memory Stick timing, so networking and
storage behaviour must also be checked on hardware.

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
| `scripts/dev.sh romm-lan` | The same on every interface, for a PSP on the LAN; the address is detected, or set `SKIFF_LAN_IP` |
| `scripts/dev.sh romm-check` | Checks what the client relies on (below); ends with `SKIFF INTEGRATION SERVER OK` |
| `scripts/dev.sh romm-down` | Stops it and deletes its volumes |

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
commit them, and do not reuse them anywhere else. With `romm-lan` the server is reachable by
anyone on your network until `romm-down`.

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

The **Skiff KIRK probe** is installed too; run it only when working on entropy (see
[KIRK probe](#kirk-probe)).

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

## Test data rules

Homebrew, public domain or synthetic only. No commercial games, BIOS files, real saves, tokens or
keys, ever.
