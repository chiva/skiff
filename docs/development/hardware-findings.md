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
| Memory Stick | 64 GB, 84% full during the resume and speed runs |
| Wi-Fi | open access point on channel 6, signal 95–100%, WLAN Power Save off unless stated (the W7 resume runs did not record it) |
| Server | the [test RomM](testing.md#integration-server) (RomM 5.3.1 behind Caddy) on a Mac on the same LAN; TCP connect ≈10 ms |
| Dates | 2026-10-04 to 2026-10-08 |

## Decisions at a glance

| Question | Finding | Decision |
|---|---|---|
| Where TLS gets its randomness | KIRK's output differed on 9 launches, power cycles included; 5 ms per request | KIRK through ARK alone, nothing mixed in ([entropy](#entropy)) |
| CPU clock while online | HTTPS 349 → 462 KB/s at 333 MHz; the clock only changes before the network modules load | 333 MHz while the network is up, 222 MHz after ([throughput](#throughput)) |
| Cipher | ChaCha20 about 340 KB/s, AES-128-GCM 172–183 KB/s | ChaCha20 first, AES-GCM as fallback (Mbed TLS's default order) |
| Client certificate type | handshake 0.69 s with ECDSA P-256, 2.07 s with RSA-2048 | the guide recommends ECDSA P-256 ([TLS](#tls)) |
| curl buffer, socket receive buffer | no effect beyond noise; a 128 KB receive buffer broke the network stack | defaults; never raise `SO_RCVBUF` to 128 KB |
| How downloads write | while the Memory Stick writes, Wi-Fi data stops arriving; 64 MiB took 221 s with 128 KB writes, 160 s with 1 MiB writes, 211 s with a writer thread | 1 MiB writes on the download's thread, no writer thread ([download speed](#download-speed)) |
| Integrity check | zlib CRC-32 33 MB/s, MD5 11 MB/s, SHA-1 8.2 MB/s | CRC-32 computed inline, against RomM's |
| Warm-up after joining, WLAN Power Save | Power Save adds 100–200 ms jitter to some requests, no throughput loss | no warm-up; both settings supported |
| Network picker | a saved profile joins without it in 7.3 s | join the profile; the picker is the fallback ([Wi-Fi](#wi-fi-and-memory)) |
| Font | Latin font 208 KB of heap; with Japanese 2067 KB | Latin only for now ([UI](#ui)) |
| Recovery after a suspend | rejoining the profile was enough (8 s); the CPU stayed at 333 MHz; open files stopped working | rejoin, reload the modules only if that fails; open files per attempt ([resume](#resume-and-power)) |
| Checkpoint interval | syncs cost 0.24 s per 64 MiB | every 4 MiB |
| Auto Sleep during a download | no suspend in 270 s with `scePowerTick()` every 5 s | keep-awake on during jobs |
| Date for certificate checks | the C library's `time()` has no date on a PSP | Mbed TLS reads the real-time clock (`sceRtc`) |
| Worker thread stack | 11,952 of 65,536 bytes used in every run, a module reload included, in the jobs probe and in the app; it leaves 84 KB of system memory once joined | 32 KB ([worker thread](#worker-thread), [the app](#the-app)) |
| Trusted CAs | the full Mozilla bundle costs about 200 ms and 280 KB of heap per new connection | ship it whole ([the app](#the-app)) |
| Queue and the UI | the UI waited 113 ms on the queue's lock while the worker saved the queue file | two locks: the UI's reads never wait for a save |

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
| HTTPS with synchronous 128 KB Memory Stick writes | 340 KB/s (−3%) | see [Download speed](#download-speed) |

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

These are the Memory Stick on its own. During a download it is much slower, and it holds up the
network: see [Download speed](#download-speed). At 470 KB/s the CRC-32 costs about 1% of the CPU. The network probe's first download (193 KB/s)
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
- The worker thread used 4 KB of stack in PPSSPP, where TLS cannot start, and 12 KB on the PSP.

## Resume and power

[Resume probe](testing.md#resume-probe), 2026-10-06: a 64 MiB file over TLS 1.3 with ChaCha20 at
333 MHz, two runs, written in 128 KB blocks (the speeds below are why that changed: see
[Download speed](#download-speed)). Design: [Threads, power and suspend](architecture.md#threads-power-and-suspend)
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

## Download speed

The resume runs downloaded at 210–275 KB/s, against 462 KB/s for the benchmark's transport, which
writes nothing. The resume probe's `speed` scenario (`scenarios=speed`) measured each part alone,
in one session, over four runs on 2026-10-06: 64 MiB at 333 MHz.

| Measurement | Result |
|---|---|
| Memory Stick alone, 128 KB writes from a 64-byte-aligned buffer / one 8 bytes off | 12.4–13.2 / 10.3–11.5 MB/s (9–12 ms per write); no slower as the file grows |
| Memory Stick alone, one 128 KB write every 300 ms (a download's pace), Wi-Fi idle | 13–14 ms per write |
| Network alone (CRC-32, nothing written), WLAN Power Save on / off | 398–448 / 457–489 KB/s |
| Whole download, 128 KB writes, WLAN Power Save on / off | 282 / 296–307 KB/s; **49 ms per write** |
| Whole download, 1 MiB writes | 386 KB/s, then **410 KB/s** (160 s) once the engine wrote 1 MiB itself |
| Whole download, 128 KB writes handed to a writer thread | 310 KB/s; the download's thread waited 0.8 s in total, the writer 25.4 s |

Buffer alignment, WLAN Power Save and an idle Memory Stick waking up are not the cause: a write
takes 10–14 ms in all those cases, and 49 ms only while Wi-Fi data is arriving. The writer thread
shows the cost is not the download's thread being blocked: while the Memory Stick writes, Wi-Fi
data stops arriving whichever thread writes, so the two compete for the hardware (presumably a
shared bus or driver; not verified). Every write is therefore also a pause in the transfer, which
takes about 0.1 s to pick up again (512 such pauses cost about 50 s per 64 MiB). Fewer, larger
writes keep the pauses down: with 1 MiB writes a download takes 160 s, about the network alone
(136 s) plus the Memory Stick's writing time (23 s).

The benchmark's 3% cost of writing (above) was measured at 222 MHz, where the CPU, not the radio,
was the limit, so the radio was often idle while the Memory Stick wrote.

Decision: downloads write 1 MiB at a time, on their own thread
(`SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES`). A writer thread is not worth its complexity. At 410 KB/s a
1 GB game takes about 43 minutes.

## Worker thread

[Jobs probe](testing.md#jobs-probe), 2026-10-07 and 2026-10-08: the download queue on its worker
thread (priority 0x30, the UI 0x20, stack 64 KB) fetching a 64 MiB file over TLS 1.3 at 333 MHz,
while the main thread draws a frame every vertical blank. Six runs turned the Wi-Fi switch off and
on, five of them also suspended the PSP, and one was quit with HOME and relaunched. Design:
[Threads, power and suspend](architecture.md#threads-power-and-suspend).

| Item | Value |
|---|---|
| Worker stack used | 11,952 of 65,536 bytes, the same in every run, including one that reloaded the network modules (PPSSPP, without TLS: 4,076) |
| System memory once joined / with the worker started | 148 KB (largest block 80 KB) / 84 KB (80 KB), unchanged while downloading |
| Heap in use once the worker started | 405 KB with the UI (its font included), 248 KB without |
| Drawing a frame (text, progress bar) | mean 1.4–1.5 ms, longest 26 ms |
| Download speed, per attempt | 345–445 KB/s while drawing (once 218 KB/s after a suspend, not seen again), 431–489 KB/s without; on 2026-10-08 a download without drawing ran at 359 KB/s, and two hours later at 480 KB/s |
| Longest wait for the queue's lock | 113 ms at the end of a job (the save) with one lock; 0 ms with two |
| Stopping the worker | 210–247 ms |
| Wi-Fi switch off: noticed / bytes again after switching on | 150–684 ms / 6.4–12.8 s |
| Suspend: bytes again after waking | 11.7–16.8 s |
| HOME → Quit at 65%, then relaunch | the job resumed from its `.part` file and finished; CRC-32 matches |

Drawing costs about 1.4 ms of each 16.7 ms frame. Speed between runs varied more than that (the
signal moved between 75% and 100% within single runs), so a comparison of two runs cannot show
it, and the download stays limited by the radio, not the CPU.

Rejoining the access point failed in about a third of recoveries, after either interruption: 2 of
5 suspends and 2 of 6 Wi-Fi switch tests, with `sceNetApctlConnect` returning 0x80410106 or
0x80410D16; once a module reload failed too. The queue's recovery (rejoin, then reload, then retry
after a pause) brought every download back.

Before `skiff.log` kept refused lines, the line written just after waking was lost in all three
runs that suspended; it is kept now.

The power callback reaches the program about 2.2 s after the frame that spans a sleep, so a
check that something "happened during a suspend" has to wait that long for it.

## The app

[The app on a PSP](testing.md#the-app-on-a-psp), 2026-10-09 (row A1): the app against the test
RomM with a 67-ROM library, `ca_file` set to the Mozilla bundle plus the test CA, `[log] level =
debug`. It paired, browsed all three pages, downloaded four games (64 MiB, 1 MiB, 256 KiB, 2 KiB)
through a Wi-Fi switch test, a suspend, the HOME menu, HOME → Quit and a relaunch, and replaced an
installed copy. The launch check it downloaded, a home-made disc image as `.iso` and `.cso`, booted
from the XMB through ARK's ISO loader and read its whole pattern file back.

| Item | Value |
|---|---|
| First request on a new connection (handshake, CA bundle parsed) | 805 and 761 ms; 590 ms without the bundle ([TLS](#tls)) |
| Heap before / after the first connection | 752 / 1105 KB; 2.4–2.5 MB while the worker downloads |
| System memory with the worker started | 84 KB free (largest block 80 KB), 148 KB before |
| Worker stack used | 11,952 of 65,536 bytes, as in the jobs probe |
| Library page request (25 ROMs, 40–61 KB of JSON) | 125–438 ms |
| Time between frames | mean 16 ms; at most 266 ms while downloading; 1.0 and 2.6 s once each while library pages arrived; 1.75 s on one slow pairing check |
| Wi-Fi switch off and on / suspend | bytes again after about 23 s (a rejoin failed, the reload worked) / 16 s |
| HOME → Quit, relaunch | joined the saved connection without the picker in 7.5 s; the download resumed |

RomM 5.3.1's pairing page reads the code only from its address (`/pair/device?user_code=`): it
has no field to type the code into, so Skiff shows the address with the code in it.

### A real RomM, with the browsing thread

2026-10-10, the same PSP-1000: `main` after #65–#67, unzipped over 0.2.0 as a player updates,
against the maintainer's own RomM 5.3.1 over public HTTPS with the bundled `cacert.pem` (no
`ca_file`), `[log] level = debug`. Browsing, scrolling past loaded rows, Back from a game still
loading, a download, all as expected.

| Item | Value |
|---|---|
| Time between frames while browsing | mean 16 ms, longest 16 ms, in every 10 s window with requests |
| Library page request (25 ROMs, 150–171 KB of JSON) | 395–691 ms; the first request on a new connection 2.3 s |
| Name lookup (`skiff_psp_net_resolve()`) | 4–11 ms |
| System memory with the worker and the browsing thread | 84 KB free (largest block 80 KB) |
| Browsing thread stack used | 9,328 of 32,768 bytes |
| Worker stack used | 12,424 of 32,768 bytes |
| Frames still over 100 ms | 3.2 s at launch (the first free-space query); 467 ms while TLS starts; 116–233 ms around queueing a download |

## Open questions

- ~~**Stalls while browsing.**~~ Answered: the 2.6 s frame was the first free-space query (now at
  launch, #59), and requests to RomM now run on the browsing thread (#67): no frame over 16 ms
  while browsing on 2026-10-10. Left: the launch's free-space query, TLS start (467 ms) and
  queueing a download (up to 233 ms) still block a frame.
- **Rejoin failures.** About a third of rejoins failed (0x80410106, 0x80410D16), after a suspend
  or the Wi-Fi switch; reloading the modules straight away would save the 2–7 s a failed rejoin
  costs. Watch the app's logs.
- **Slow first request.** The 3.3 s first request after joining was seen once and not reproduced;
  watch for it in the app's logs. With 0.2.0 against a real RomM over public HTTPS (2026-10-09),
  the first request after a launch failed only after 4.5 minutes (`SKIFF_ERR_NET_TIMEOUT`), with
  the screen frozen; the retry 24 minutes later took 1 s. The connect and stall timeouts (10 and
  30 s) cannot add up to that; curl's name lookup, which cannot time out on the PSP, can. Lookups
  now go through the firmware's resolver with a timeout, and `[log] level = debug` logs each
  lookup's time (`resolve <host>: <ms> ms`), so the next occurrence names its cause. Not seen again
  on 2026-10-10 (lookups 4–11 ms).
- **Other consoles.** No numbers yet for the PSP-2000, 3000 or Go (64 MB), or for other access
  points.
