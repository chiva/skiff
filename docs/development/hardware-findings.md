# Hardware findings

Phase 1 measured, on a real PSP, what an emulator cannot show: random numbers, memory, Wi-Fi,
TLS, the Memory Stick, and how a download survives interruptions. This page has the numbers and
the decisions they settle. Each number comes from a probe EBOOT in `tests/` that can be run again
(see [Testing](testing.md#hardware-tier)); the design itself is described in
[Architecture](architecture.md).

All of it comes from one console. Other models and access points will differ, so read the numbers
as orders of magnitude.

## Test setup

| | |
|---|---|
| Console | PSP-1000 (32 MB RAM), firmware 6.61 with ARK-5, on AC power |
| Memory Stick | 64 GB, 84% full during the resume runs |
| Wi-Fi | open access point on channel 6, signal 95–100%, WLAN Power Save off unless stated |
| Server | the [test RomM](testing.md#integration-server) (RomM 5.3.1 behind Caddy) on a Mac on the same LAN; TCP connect ≈10 ms |
| Dates | 2026-10-04 to 2026-10-06 |

## Decisions at a glance

| Question | Finding | Decision |
|---|---|---|
| Where TLS gets its randomness | KIRK's output differed on 9 launches, power cycles included; 5 ms per request | KIRK through ARK alone, nothing mixed in ([entropy](#entropy)) |
| CPU clock while online | HTTPS 349 → 462 KB/s at 333 MHz; the clock only changes before the network modules load | 333 MHz while the network is up, 222 MHz after ([throughput](#throughput)) |
| Cipher | ChaCha20 about 340 KB/s, AES-128-GCM 172–183 KB/s | ChaCha20 first, AES-GCM as fallback (Mbed TLS's default order) |
| Client certificate type | handshake 0.69 s with ECDSA P-256, 2.07 s with RSA-2048 | the guide recommends ECDSA P-256 ([TLS](#tls)) |
| curl buffer, socket receive buffer | no effect beyond noise; a 128 KB receive buffer broke the network stack | defaults; never raise `SO_RCVBUF` to 128 KB |
| Writer thread for downloads | Memory Stick writes 9–13 MB/s; writing while downloading cost 3% | none: synchronous 128 KB writes ([Memory Stick](#memory-stick-and-hashing)) |
| Integrity check | zlib CRC-32 33 MB/s, MD5 11 MB/s, SHA-1 8.2 MB/s | CRC-32 computed inline, against RomM's |
| Warm-up after joining, WLAN Power Save | Power Save adds 100–200 ms jitter to some requests, no throughput loss | no warm-up; both settings supported |
| Network picker | a saved profile joins without it in 7.3 s | join the profile; the picker is the fallback ([Wi-Fi](#wi-fi-and-memory)) |
| Font | Latin font 208 KB of heap; with Japanese 2067 KB | Latin only for now ([UI](#ui)) |
| Recovery after a suspend | rejoining the profile was enough (8 s); the CPU stayed at 333 MHz; open files stopped working | rejoin, reload the modules only if that fails; open files per attempt ([resume](#resume-and-power)) |
| Checkpoint interval | syncs cost 0.24 s per 64 MiB | every 4 MiB |
| Auto Sleep during a download | no suspend in 270 s with `scePowerTick()` every 5 s | keep-awake on during jobs |
| Date for certificate checks | the C library's `time()` has no date on a PSP | Mbed TLS reads the real-time clock (`sceRtc`) |

## Entropy

[KIRK probe](testing.md#kirk-probe), 2026-10-04 and 2026-10-05. Design:
[Randomness for TLS](architecture.md#randomness-for-tls).

| Item | Value |
|---|---|
| 128-byte request (what Mbed TLS asks for), mean / max | 4.94 / 5.11 ms (≈154 µs per `sctrlKernelRand()` call) |
| `psa_crypto_init()` (seeds Mbed TLS once per session) | 10.7–11.0 ms |
| First values across 9 launches, power cycles included | all different |
| 1024 values in one run | no repeats; bit and byte counts inside 5σ (χ² 257.4) |
| Uptime when the probe started, every launch | 5.92–5.95 s |

The system timer restarts at every launch, so a seed taken from it would repeat; KIRK's sequence
does not, so nothing is mixed into it.

## Wi-Fi and memory

[Network probe](testing.md#network-probe) and [UI prototype](testing.md#ui-prototype),
2026-10-05.

| Item | Value |
|---|---|
| System memory free before the network modules (largest block) | 764 KB (512 KB) |
| After loading them (common + inet, apctl) | 164 KB (84 KB): they take 600 KB |
| Once joined | 148 KB (80 KB) |
| After unloading | 764 KB again |
| Heap used by TLS at its peak | 72 KB (TLS uses the heap, not system memory) |
| Joining a saved profile (`sceNetApctlConnect`), no picker | 7.3–7.4 s, once 11.1 s |
| Network picker (`sceUtilityNetconf`) from the GU loop | works; disconnecting takes 1050 ms |

Skiff's heap takes all but 1 MB of user memory, so the network modules leave about 150 KB of
system memory, in blocks of at most 80 KB. Thread stacks come from there: the worker thread must
fit, or the heap must shrink (see [open questions](#open-questions)).

## TLS

Network probe and [benchmark](testing.md#benchmark), 2026-10-05; TLS 1.3 with
ChaCha20-Poly1305, negotiated with Caddy.

| Item | Value |
|---|---|
| Handshake, median of 5, 222 MHz: no client certificate / ECDSA P-256 / RSA-2048 | 590 / 692 / 2071 ms |
| Handshake at 222 / 333 MHz | 595 / 400 ms |
| TLS 1.2 against 1.3, same cipher | same handshake time |
| A second request on a kept connection | 48 ms, no new handshake |
| The C library's `time()` | time of day only (36834 s at 09:13 UTC): every certificate looked "from the future" |
| PSP clock set to 2000 | reported as a wrong date (`SKIFF_ERR_NET_TLS_CLOCK`, 108) |

`time()` is why Mbed TLS reads the date from the real-time clock instead
([The PSP's clock](architecture.md#the-psps-clock)).

## Throughput

[Benchmark](testing.md#benchmark), 2026-10-05: a 4 MiB file, 3 runs each, median, no Memory Stick
writes unless stated.

| Download | 222 MHz | 333 MHz |
|---|---|---|
| HTTPS, ChaCha20 (CPU busy) | 339–376 KB/s (80–85%) | 469–487 KB/s (≈72%) |
| Plain HTTP | 438–472 KB/s | 420–492 KB/s |
| Skiff's transport (curl defaults) | 349 KB/s | 462–467 KB/s |
| HTTPS, AES-128-GCM, TLS 1.3 / 1.2 | 172 / 183 KB/s | — |
| HTTPS with synchronous 128 KB Memory Stick writes | 340 KB/s (−3%) | — |

At 222 MHz the CPU is the limit; at 333 MHz HTTPS and plain HTTP are equal, so the limit is the
802.11b radio, at about 500 KB/s (4 Mbit/s). At 460 KB/s a 700 MB game takes about 26 minutes.

| Decryption on the CPU, 222 MHz | Speed |
|---|---|
| ChaCha20-Poly1305 | 3014 KB/s (72 cycles per byte) |
| AES-128-GCM | 368 KB/s (588 cycles per byte) |

Also measured, without effect beyond noise: curl's receive buffer from 16 to 512 KB, and a socket
receive buffer of 32 or 64 KB. A 128 KB receive buffer broke the network stack for the rest of the
run (156 KB of system memory left). WLAN Power Save: with it off, connects took 7–17 ms; with it
on, most took under 10 ms but some 178–208 ms, and a request after 15 s idle took 162 ms instead of
53 ms. Throughput was the same either way.

The first request after joining took 3.3 s once (later ones about 0.7 s); the benchmark did not
see it again in 4 runs.

## Memory Stick and hashing

Benchmark, 2026-10-05, 222 MHz.

| Item | Value |
|---|---|
| Writes in 16–512 KB blocks (16 MiB) | 9–13 MB/s |
| Reads | 9–11 MB/s |
| zlib CRC-32 / table CRC-32 / bitwise CRC-32 | 33 MB/s / 4.8 MB/s / 733 KB/s |
| MD5 / SHA-1 | 11 / 8.2 MB/s |

At 470 KB/s the CRC-32 costs about 1% of the CPU. The network probe's first download (193 KB/s)
was held back by the probe's own checksum (598 KB/s), not by the network.

## UI

UI prototype, 2026-10-05. Design: [UI: GU + intraFont](architecture.md#ui-gu--intrafont).

| Item | Value |
|---|---|
| Latin firmware font (`ltn0.pgf`, medium cache) | 208 KB of heap, loaded in 49 ms |
| Latin and Japanese (`jpn0.pgf`) | 2067 KB of heap; the Japanese font loads in 967 ms |
| A 20-item list, drawn and rendered | mean 4.8 ms, longest 27.8 ms; 1 of 1743 frames over 16.7 ms |
| On-screen keyboard, network picker | both open and close from the GU loop |

## Where PPSSPP differs

The emulator tier runs every probe in CI, but these differences only showed on hardware:

- PPSSPP has no ARK, so KIRK, TLS and everything past `curl_global_init()` can only be checked
  there for the refusal path.
- The network modules take 169 KB of system memory in PPSSPP and 600 KB on the PSP.
- PPSSPP's replacement fonts used 4.9 MB of heap with Japanese; the firmware's fonts use 2067 KB,
  and the Latin font alone 208 KB.
- intraFont turns depth testing back on after printing. Without a depth buffer the PSP then
  discards rectangles drawn afterwards; PPSSPP draws them.
- A suspend cannot be tested in PPSSPP; on the PSP it invalidates open files.

## Resume and power

[Resume probe](testing.md#resume-probe), 2026-10-06: a 64 MiB file over TLS 1.3 with ChaCha20 at
333 MHz, two runs. Design: [Threads, power and suspend](architecture.md#threads-power-and-suspend)
and [Downloads and storage](architecture.md#downloads-and-storage).

| Scenario | What happened | Speed |
|---|---|---|
| Restart (new connection at 40%) | resumed with 206 from the saved offset; CRC-32 matches | 210 / 275 KB/s |
| Wi-Fi switch off at 20%, on again | noticed 1.7 s after the last byte; profile rejoined in 13.1 s; 206; CRC-32 matches | 231 KB/s |
| Suspend at 20%, wake after 5 s | the stop hook fired on its first poll after waking; rejoining was enough (8.1 s, no module reload); CPU still at 333 MHz; 206; CRC-32 matches | 235 KB/s |
| HOME menu for 10 s | the download kept going | 246 KB/s |
| Auto Sleep at its shortest, no input | 0 suspends in 270 s | 243 KB/s |
| Clock after unloading the network | back to 222/111 MHz | — |

A suspend also invalidates files left open on the Memory Stick: the probe's `result.txt` stopped
receiving lines after waking. Every download attempt therefore opens and closes its own files.

Storage time in the second run (238 s in total):

| Operation | Calls | Total | Longest |
|---|---|---|---|
| Write (128 KB) | 532 | 26.3 s | 225 ms |
| Sync | 35 | 0.24 s | 20 ms |
| Open, close, rename, remove | 50 | 0.34 s | 22 ms |

## Open questions

- **Download speed.** The resume probe downloaded at 210–275 KB/s, against 462 KB/s for the
  benchmark's transport, and both the network and the Memory Stick were slower. Writes averaged
  about 2.5 MB/s (49 ms per 128 KB, 11% of the run), where the benchmark measured 9–13 MB/s on the
  same Memory Stick; without the write time, the network delivered about 310 KB/s. The runs
  differ in file size (64 MiB against 4 MiB), in day and radio conditions (the resume probe does
  not log the signal), and in a pause of 5.4–6 s in every resume run, probably the wait for the
  first byte (the probe now logs the two apart). Which of these matters is unknown. Settle it
  before `jobs/` fixes its design: run the benchmark's transport with the 64 MiB file in the same
  session as the resume probe, log the signal, and time writes by file offset. If writes stay
  this slow, a writer thread would win back their 11%.
- **Memory for threads.** Once joined, about 148 KB of system memory is free, in blocks of at most
  80 KB. Size the worker thread's stack, and `PSP_HEAP_SIZE_KB` if needed, when `jobs/` adds the
  thread.
- **Slow first request.** The 3.3 s first request after joining was seen once and not reproduced;
  watch for it in the app's logs.
- **Other consoles.** No numbers yet for the PSP-2000, 3000 or Go (64 MB), or for other access
  points.
