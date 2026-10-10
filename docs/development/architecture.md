# Architecture

## Goals and constraints

- Runs on a PSP-1000: 333 MHz MIPS (Allegrex), 32 MB RAM, 802.11b Wi-Fi, FAT32 Memory Stick.
  Later models have 64 MB; Skiff sets `MEMSIZE=1` in PARAM.SFO to use it (PPSSPP reports a 57 MB
  heap with it).
- Talks to RomM 5.3+ over its REST API, with HTTPS and optional client certificates.
- Usable by people who are not technical: every failure becomes a sentence and a code, in the
  PSP's system language.
- Extensible to other platforms (PS1, emulated systems) without touching the core.
- A download can take most of an hour (a PSP-1000 downloads at about 3.3 Mbit/s over HTTPS, see
  [Hardware findings](hardware-findings.md)), so everything is designed around long,
  interruptible transfers.

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

Status: `app/` (screens and state machine, host-tested, and run on the PSP by `src/platform/psp/app_main.c`), `core/`, `config/` (`config.ini`), `log/` (`skiff.log`), `i18n/` (English and Spanish
text), `net/` (transport, TLS entropy source), `romm/` (version check, platforms, ROM pages),
`storage/` (the storage seam, logical roots, free space, safe names), `cover/` (PNG covers decoded
and scaled down for the screen, see [Covers](#covers)), `install/` (the PSP
installer and `installed.json`), `jobs/` (resumable downloads, the download queue and its retry
policy), `ui/` (input, list, text fitting and progress models) and `platform/psp/` (lifecycle,
network stack, TLS hooks, the GU renderer, the system dialogs) exist. The other layers arrive with the
[roadmap](roadmap.md) phases that need them.

## Threads, power and suspend

- **UI thread**: input, drawing, system dialogs. Never blocks on the network or the Memory Stick.
  A Wi-Fi join takes 7 to 13 s on a PSP, so the UI thread starts it and looks at it once per frame
  (`skiff_psp_net_connect_start()` and `skiff_psp_net_connect_poll()`, `src/platform/psp/net_psp.h`)
  while it keeps drawing; the worker's blocking `skiff_psp_net_connect()` is a loop over the same
  two calls. The profile to join is the one the network picker last connected through, found by
  its name (`skiff_psp_net_connected_profile()`; with two profiles of the same name, the first).
- **Browsing thread**: runs the app's requests to RomM (connection check, platform, library pages,
  a game's details, pairing), one at a time (`src/platform/psp/call_psp.h`), so a slow network
  never freezes the screen. The app copies a request's inputs into its call before it starts and
  reads the results once the UI sees it done (`src/app/app_call.c`); the thread touches nothing
  else but the browse client, which is not replaced while a call runs. Leaving what a request was
  for (Back from a game's details, a pairing given up) drops its result and asks its transfer to
  stop. Settings that would replace the client (a new server, pairing again) wait for the call.
  It starts with the first request, at the worker's priority, with a 32 KB stack.
- **Worker thread**: runs one job at a time (download, save sync) from a persistent queue
  (`include/skiff/jobs.h`, `skiff_jobs_run_one()`), and posts progress and results to the UI as
  events: a small ring of state and recovery events, oldest dropped if the UI falls behind, and
  only the latest progress per job (bytes, size, speed). Both threads share the queue through lock
  hooks and two locks. The state lock guards what the UI reads every frame (the jobs, events,
  progress) and is held only for moments. The commit lock is held for a whole change that saves
  the queue file: the change is made to a copy, the copy is saved without the state lock, and only
  then does the UI see it. So the UI never waits for the Memory Stick (on hardware it waited 113 ms
  for a save at the end of a job before this split), and the queue never shows what a restart would
  undo. The runner holds neither lock during a transfer. Modules hold no global mutable state, so
  a job owns its transport and file handles.
- The PSP kernel schedules by priority without time-slicing equal priorities, so the worker runs
  at a lower priority than the UI (`SKIFF_PSP_WORKER_PRIORITY` 0x30 against the main thread's 0x20,
  `src/platform/psp/jobs_psp.h`): the UI waits for every vertical blank and draws first, and the
  worker downloads in the time left. The worker's stack (`SKIFF_PSP_WORKER_STACK_BYTES`, 32 KB) comes
  from the same memory as the network modules, about 148 KB once joined in blocks of at most
  80 KB; on a PSP the worker used 12 KB of it, downloads and recoveries included, in the jobs probe
  and in the app
  ([hardware findings](hardware-findings.md#worker-thread)). The queue's two locks and the log each
  have their own semaphore-backed mutex (the queue logs while it holds its commit lock); rejoining or reloading the
  network can take a further lock other threads share. Quitting asks the runner to stop and waits
  up to 5 s for the thread; a join still in progress is left to the process exit.
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
- **Name lookups** are bounded: curl on the PSP has no resolver thread and runs without signals,
  so its own lookup cannot time out (a 0.2.0 request waited 4.5 minutes). Every PSP transport
  looks names up through `skiff_psp_net_resolve()` (5 s per try, 2 more tries) and hands curl the
  address (`CURLOPT_RESOLVE`), kept until a request fails. The screen's requests also end after
  60 s in all; downloads have only the stall timeout.
- **Retry policy** (`src/jobs/runner.c`): an error a reconnect cannot fix (a RomM refusal, a full
  Memory Stick, a checksum mismatch) fails the job at once. With the Wi-Fi switch off the runner
  waits for it however long it takes, and that never counts against the job. Any other network
  failure is retried: at once when the attempt received bytes, otherwise after 1, 2, 4, 8 and
  16 s, rejoining first when the network is gone and always after a suspend. Six attempts in a row
  that receive nothing (a failed rejoin counts as one) fail the job; its `.part` file stays, so the
  player's retry resumes. A cancel or a quit stops the transfer within about a second: a cancelled
  job's partial files are deleted, a job interrupted by quitting stays queued.
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

## The app

`include/skiff/app.h` (`src/app/`) is the state machine the player walks through, portable and
tested on the host with a fake platform: STARTING (read `config.ini`, start `skiff.log` at its
`[log] level`, the queue and `installed.json`), SERVER (no address yet: the on-screen keyboard),
CONNECTING (join the Wi-Fi, start TLS, the heartbeat), PAIR, LIBRARY, DETAILS, the downloads
screen and Settings, plus an error or notice screen and a confirmation. The platform reads the
buttons, draws the view the app describes (text already fitted and wrapped), runs the system
dialogs and the network, and reaches it through `skiff_app_env`.

- **Nothing blocks a frame on the network.** Requests to RomM run on the browsing thread (see
  [Threads](#threads-power-and-suspend)) and joining the Wi-Fi (7 to 13 s) is polled, so the
  screen keeps answering: the library scrolls while a page loads, and Back leaves a game whose
  details are still on their way. The browsing thread became affordable once the worker's stack
  shrank from 64 to 32 KB (0.2.0 measured 12 KB used).
- **The Wi-Fi connection** is picked once with the system's network picker and saved as
  `[network] profile`; later launches join it without asking, and a failed join offers the picker
  again. The network stays up until Skiff quits.
- **Startup order**: heartbeat first (205 below RomM 5.3; a newer release line gets a notice once,
  remembered as `[skiff] romm_notice`), then pairing when there is no token, then the `psp`
  platform. The worker starts once RomM answered, with its own copies of the server address, token
  and transport settings, so Settings can change them while it runs.
- **Before a download is queued**: the installer can take the file (one `.iso`, `.cso` or `.zso`,
  a usable name), the queue has room (64 unfinished jobs; the player is told to clear finished
  ones), `installed.json` has room (512 records), and a game already installed asks "Replace your
  installed copy?". `games:` is created first. Downloads already queued count against the 512
  records too, since each will need one.
- **Settings**: a new server address clears the token and RomM's device id (they belong to the old
  server) and pairs again. The old server's downloads cannot run against the new one (other ROM
  ids, other files), and neither does what Skiff installed from it say anything about the new
  one (a ROM id and file name may match another game there). So the player confirms, and the
  change goes in an order that never lets either act on the new server: stop the worker, cancel
  the downloads, drop the `installed.json` records (the games stay, protected as copied by hand),
  save `config.ini`, then rebuild the queue and the client. A worker that will not stop in time changes nothing; a failed cancel or
  save keeps the old server and starts a new worker for it. "Pair again" keeps the address and
  the downloads and replaces the token, restarting the worker with it.
- **installed.json is saved off the lock the UI reads**: the worker records a finished download
  under the manifest lock, copies the manifest, and saves the copy without it, so drawing the
  library never waits for the Memory Stick.
- **On the PSP** (`src/platform/psp/app_psp.h`, `app_main.c`): one frame is read the buttons,
  update the app, draw its view, run the system dialog it asks for, present. Five mutexes are
  created at startup: the log's, the queue's two, `installed.json`'s and the network's. The network
  stack is used by one thread at a time, so the UI holds the network lock from the start of a join
  to its end, and while the network picker is open, taking it without waiting (while the worker
  rejoins, up to 30 s, the screen keeps drawing); the worker takes it to rejoin or reload. Each
  request to RomM from the UI holds it too, so the worker never tears the network down under one;
  one made while the worker recovers fails as a lost connection, and Retry joins again. Quitting
  stops the worker (5 s); only when it stopped are the app, the network (after a disconnect that
  succeeded), TLS and the mutexes released, otherwise the process exit does it. A system dialog
  that will not close ends the app the same way.
- **Secrets**: the token and every custom header value of at least 8 characters are registered
  with the log; a shorter value cannot be found reliably and registering it would withhold every
  line.

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
  third-party licences (`scripts/psp-packages.txt`).

Phase 1 confirmed this with a prototype on hardware (`tests/prototype/ui_proto.c`, see
[Testing](testing.md#ui-prototype)). The prototype still draws a Japanese line and measures both
fonts, so the cost of adding Japanese later is known.

The UI is split along the platform line:

- `include/skiff/ui.h` (`src/ui/`, host-tested) holds what is not drawing. Buttons become actions:
  confirm and back follow the console's own setting, so Circle confirms on Japanese consoles and
  Cross elsewhere. A held d-pad direction or shoulder button repeats after 20 frames, then every 4.
  It also holds a scrolling list (up and down wrap, L and R page and stop at the ends), and text cut
  to a width between UTF-8 characters with `...`, since the Latin firmware font has no `…`.
  Measuring text costs time on the PSP, so labels are cut once, when they change, not every frame.
  The rest is download progress (percent, a rate smoothed over 2-second windows, time left) and
  sizes and durations as the player reads them (`1,5 GB` in Spanish, `2 h 05 min`).
- `src/platform/psp/ui_psp.h` draws those models with GU and intraFont: a header and a footer of
  button hints with the PlayStation symbols, a list with its highlight and scroll bar, a progress
  bar, and the dim backdrop under a system dialog. It disables the depth test before every
  rectangle and writes vertices back from the data cache (AGENTS.md). The UI prototype is drawn by
  it, so the headless run in CI exercises the renderer.
- `src/platform/psp/dialog_psp.h` runs the on-screen keyboard and the network picker one step per
  frame: the caller draws its screen and the backdrop, then the dialog's update lets the system draw
  over them. It keeps what the prototype learnt on hardware: a refused close is asked again every
  frame, and a dialog still there 10 s after being asked to close is given up on (it may own the
  screen and buttons, so the app ends there); the network modules stay loaded under an open or
  stuck picker. The keyboard works in UTF-16; `skiff/ui.h` converts to and from UTF-8, with invalid
  input replaced by U+FFFD, so the conversion is host-tested and checked by the self-test on
  the PSP's signed `char`. The UI prototype runs its dialogs through this module.

## Errors and languages

`include/skiff/error.h` holds one X-macro table that generates the `skiff_err` enum, the stable
names and the English messages. Codes are grouped by layer (1xx network, 2xx RomM, 3xx storage,
4xx config) and never renumbered, because players quote them in bug reports. The
[troubleshooting guide](../guide/troubleshooting.md) documents every code, and a pre-commit check
(`scripts/check-error-docs.sh`) fails if a code is missing from it.

What the player reads comes from `include/skiff/i18n.h` (`src/i18n/`), in English or Spanish:
Spanish when the PSP's system language is Spanish, English otherwise. Both languages live in
tables a missing string cannot get past:

- UI strings are one X-macro table with the English and Spanish text side by side, so a row
  without its translation does not compile. The unit test checks each row: both texts use exactly
  the placeholders the row declares, and only characters the Latin firmware font draws (ASCII and
  Latin-1).
- English error sentences are `error.h`'s messages, so there is one source. Spanish ones are a
  table naming each `error.h` code once: a misspelt code, or a code added to `error.h` without a
  Spanish row, does not compile.
- Placeholders are `{1}` to `{9}`, positional so a translation can reorder them, and filled only
  with strings. A translation therefore cannot introduce a printf conversion that does not match
  its argument. Numbers are formatted first, with the language's decimal separator.

An error is shown as its sentence followed by its code, e.g. "Could not reach the RomM server
[102]", which players quote in bug reports. Logs always use the stable name and number, never
translated text.

## Configuration

`config/` reads and writes `PSP/GAME/Skiff/config.ini` (`include/skiff/config.h`). The player may
edit it on a computer, and Skiff writes it too (pairing saves the token, Settings the address), so
there is one file and Skiff changes only the line of the key it sets: comments, keys it does not
know and the player's order survive.

| Section | Key | Rule |
|---|---|---|
| `[skiff]` | `version` | Schema version; absent means 1, a newer one is refused (402) |
| `[server]` | `url` | Explicit `http://` or `https://`, no blanks (the transport's own rule) |
| `[server]` | `ca_file` | A file name in the Skiff folder; empty for the bundled Mozilla CAs (`cacert.pem`), which a set one replaces |
| `[auth]` | `token` | Printable ASCII without blanks |
| `[auth]` | `device_identifier`, `device_id` | Printable ASCII without blanks; written by pairing (below) |
| `[mtls]` | `cert_file`, `key_file` | File names in the Skiff folder; one without the other is 401 |
| `[headers]` | any name | Sent on every request; not `Authorization`, `Host`, `Range` or `If-Range` |
| `[log]` | `level` | `error`, `warn`, `info` or `debug` for `skiff.log`; `info` when unset |
| `[network]` | `profile` | The Network Settings connection (1-10) Skiff joins; Skiff saves the one picked in the network picker on the first launch |
| `[skiff]` | `romm_notice` | The newer RomM release line the player was already told about; written by Skiff |

- **Parsing**: a UTF-8 byte-order mark and CRLF line endings are accepted (Notepad writes both);
  names ignore case; `#` or `;` starts a comment only at the start of a line, because a token or a
  proxy secret may contain `#`. An empty value means the setting is not set.
- **Refusals name the setting**: every error carries the line, section and key, so the player is
  told which one to fix (400 damaged line, 401 missing key, 402 bad value). A value that does not
  fit is refused, never cut; a setting given twice is refused rather than one silently winning.
  Unknown keys are ignored, so an older Skiff reads a newer file, and the first one is kept for a
  log warning, since a typo such as `ca-file` would otherwise do nothing.
- **Saving**: `config.ini` is replaced whole with the storage seam's crash-safe replacement (see
  [Small files replaced whole](#downloads-and-storage)), and an edit is parsed before it is saved,
  so a saved file always loads.
- **Schema changes**: the `version` key exists from the first release; migrations arrive with the
  first change to the schema.

## Logging

`log/` (`include/skiff/log.h`) writes `PSP/GAME/Skiff/skiff.log`, the file the bug report form
asks for. A logger is an object the app creates and passes to the layers that log (`jobs/`,
`app/`), not a global; `net/` and `romm/` return errors and leave logging to their callers. Lines
read `2026-10-06 12:34:56.789Z W jobs: text`: the time comes from a clock the platform supplies
(the real-time clock on the PSP, since `time()` has no date there), the level is one of error,
warn, info and debug, and errors appear by stable name and number, never translated.

- **Batched writes**: lines wait in an 8 KB buffer, because every Memory Stick write during a
  download also pauses Wi-Fi reception ([Download speed](hardware-findings.md#download-speed)). A
  batch is written when the buffer is full, after every warning or error (so they survive a crash),
  when the logger is destroyed, and when `jobs/` asks for it right after the download engine's own
  1 MiB write, while reception is paused anyway.
- **Suspend-safe and durable**: each batch opens the file, writes, syncs and closes it, so no
  handle is left open for a suspend to invalidate and a crash cannot lose a warning already
  written. A batch that fails is tried once more at the same offset (a handle lost
  to a suspend works again once reopened). One refused twice stays in the buffer and goes out with
  the next warning, error or flush, written over whatever part of it reached the file: right after
  a suspend the Memory Stick can refuse for longer than one retry, and the line logged then (why
  the download stopped) is the one a bug report needs. While the Memory Stick keeps refusing, a
  full buffer gives up its oldest lines rather than retry per line, and the batch that finally
  goes out starts with a line saying how many were lost. A part of a batch whose first lines were
  given up meanwhile cannot be written over; it stays (the storage seam cannot shorten a file) and
  is cut off from the next batch by a line break. Logging never fails its caller.
- **Size cap**: past 256 KB the log moves to `skiff.log.1`, replacing an older one, and a new file
  starts. If the move fails, the log starts over rather than grow past the cap.
- **Redaction in the logger, not at call sites**: the values that are secret (the token, the
  custom header values) are registered once with `skiff_log_add_secret()` and replaced by
  `[redacted]` wherever they appear in any line, also in a URL or a response logged by mistake
  (values nested in or overlapping one another are redacted as one stretch). Registration fails
  closed: a value the logger cannot hold (under 8 or over 255 bytes, or past 12 values) makes every
  later message show as withheld; `config.ini`'s checks keep configured values inside those
  limits. A
  line cut at its maximum length also loses any tail that could be the start of a secret.
  `skiff_log_header()` shows the value only of headers known to carry none (`Content-Length`,
  `Content-Range`, `Content-Type`, `ETag`), so an unknown proxy header is hidden by default.
  Control characters are replaced, so text from a server cannot forge log lines.

## RomM integration

| Need | RomM API |
|---|---|
| Version check | `GET /api/heartbeat` |
| Pairing | Device-code flow: `POST /api/auth/device/init` (`client_device_identifier`, `name`, `client`, `platform`, `client_version`, `requested_scopes`) gives a `device_code` and a `user_code`; the user approves in the web UI while the PSP polls `POST /api/auth/device/token` for an `access_token` and its `device_id` |
| Fallback auth | Client API token (`rmm_…`) as `Authorization: Bearer` |
| Browse | `GET /api/platforms`, then `GET /api/roms?platform_ids=…&limit=…&offset=…`, **paginated** so one JSON page stays small; a ROM's files from `GET /api/roms/{id}` |
| Favourites | The same list with `favorite=true` (and `with_files=true` when each ROM's file is needed) |
| Download | `GET /api/roms/{id}/content/{file_name}` with `Range` and `If-Range` |
| Cover | The ROM's `path_cover_small`, a file RomM's web server serves under `/assets/romm/resources/`, without a token |
| Saves | `POST /api/saves`, `GET /api/saves/{id}/content`, `POST /api/sync/negotiate`, tagged with this PSP's `device_id` |

Custom headers: `config.ini` may define extra HTTP headers sent on every request. This covers
proxy access schemes the PSP cannot otherwise satisfy — notably Cloudflare Access service tokens
(`CF-Access-Client-Id` / `CF-Access-Client-Secret`) — without a VPN or a client certificate. It is
a few lines in the transport and a cheaper alternative to mTLS for users already behind such a
proxy. Header values are secrets: redacted in logs, same caveats as the token.

Version policy (`skiff_romm_check_version()`): below 5.3, the release that brought device-code
pairing and per-file hashes, Skiff refuses to run (`SKIFF_ERR_ROMM_UNSUPPORTED_VERSION`, 205). A
newer release line than the one tested (5.3) is allowed with a one-time notice, because refusing
every untested release would break Skiff with each RomM update; a patch release gets no notice. A
version that does not start with a number (a development build) is allowed with the notice too.

The client (`include/skiff/romm.h`) reads each response into a buffer of at most 512 KiB and parses
it with cJSON 1.7.16 (pspdev's package; the host image builds the same version). A larger response
is refused (204), never cut, and so is anything that is not RomM's JSON: a proxy's login page, a cut
body, valid JSON followed by anything but blanks, a missing field, a number that is not a whole,
non-negative value below 2^53, a control byte or an escaped NUL inside a string, or a string other
than a name (a slug, a CRC, the server version) longer than its field or holding a control
character. A name (a ROM's title or file name, or a file's name) with an escaped control character,
or too long for its field, does not refuse its page: the item is listed with each control character
(C0, DEL or C1) shown as `?` and a long name cut at a UTF-8 character boundary and ended with `~`,
but it is not downloadable (a ROM's files neither), since that is not the name RomM serves the file
under. Before cJSON builds its tree, a scan counts the values the body holds (at most 32768, about
44 bytes each on the PSP): RomM's responses run near one value per 20 bytes, while a body of tiny
values would otherwise cost megabytes. A request peaks near 2 MB. ROM lists ask for 25 ROMs a page,
ordered by name: an unidentified ROM is about 2.6 KB of JSON and one with metadata several times
that, so a page stays well under the cap (cJSON's tree and its strings stay within the budget above)
and fills a screen in one request. They also turn off `with_char_index`, `with_filter_values` and
`with_rom_id_index`: RomM includes those by default, and they grow with the whole library, not the
page. A page that is not the one asked for (another offset, more ROMs than the limit or than the
total leaves, a ROM of another platform) is refused, as is a ROM returned under another id or
listing a file of another ROM. List items carry the ROM's name, file name, size and CRC-32;
`files[]` comes with `GET /api/roms/{id}`, and with a list only when it asks `with_files` (about 0.5
KB more per ROM). A ROM listed with exactly one file that reads as its own carries that file
(`skiff_romm_rom_summary.file`), so a download needs no request for its details; anything else
(several files, a file of another ROM, a broken entry) leaves the ROM listed without it, rather
than refusing the page.

Favourites are the same list with `favorite=true`: RomM keeps a player's favourites as their own
collections marked `is_favorite` (a user may have several, which RomM merges), created by its web UI
on the first favourite, and the filter needs no scope beyond `roms.read`, so Skiff's token reads
them. A player without favourites gets an empty list. RomM's collection endpoints need
`collections.read`, which Skiff does not ask for (see [Pairing](#pairing)). RomM records CRC32, MD5 and SHA-1 for every file, so the
integrity check can use any of them; its `crc_hash` is hexadecimal, read with or without leading
zeros.

Download URLs percent-encode every byte of the file name except letters, digits and `-._~`: a `#`
would otherwise end the path, a `?` start a query, a `/` split it. The URL builder takes the file
RomM listed and refuses one whose name was not usable. The heartbeat is public and is
sent without the token.

A ROM's details name its small cover (`path_cover_small`, `skiff_romm_rom.cover_path`): not an API
endpoint but a path on the server, such as
`/assets/romm/resources/roms/1/2/cover/small.png?ts=2026-10-09 23:02:27`. RomM's nginx serves those
files to anyone, so a cover request carries the custom headers but not the token, and the `ts`
query (when RomM last changed the ROM) marks the file as never changing. That query holds a raw
space, so `skiff_romm_cover_url()` percent-encodes every byte other than letters, digits and
`-._~/?=&:`. A path outside `/assets/romm/resources/`, or with a `.` or `..` segment, a backslash or
a control character, is treated as no cover, so a response cannot send Skiff elsewhere; a cover never
refuses its ROM. RomM makes the small cover by resizing the stored one to 40% (20% from 1000 px
tall) and saving it under the stored file's extension: a PNG for every metadata provider, the
upload's format for artwork uploaded in the web UI. Its WebP option writes a copy beside each cover
without changing these paths.

### Pairing

`include/skiff/romm_pairing.h` runs RomM's device-code flow, which follows RFC 8628 (the OAuth
device grant):

1. `POST /api/auth/device/init` (open, rate-limited) with this PSP's `client_device_identifier`,
   `name` "Skiff on PSP", `client` "skiff", `platform` "psp", `client_version` and the scopes Skiff
   asks for: `platforms.read` and `roms.read`, all that browsing and downloading need (checked
   against RomM 5.3.1), favourites included, so the token on the Memory Stick can only read. Save
   sync (Phase 5) needs `devices.*` and, in RomM 5.3.1, `assets.*`: it will ask the player to pair
   again, and collections (`collections.read`) wait for that pairing rather than ask for a second. RomM answers
   201 with a `device_code`, an 8-character `user_code` (letters and digits, e.g. `7EGGP3VE`), a
   `verification_path` relative to the server (`/pair/device`, also with `?user_code=`), `expires_in`
   (600 s) and `interval` (5 s). Skiff shows the code and the server's address plus the path with
   `?user_code=` (RomM 5.3.1's page reads the code only from the address, it has no field for it),
   broken before the `?` when it does not fit a line so the code stays whole, and the same address
   as a QR code beside it (`skiff_ui_qr_encode()` in `include/skiff/ui.h`, Nayuki's QR Code
   generator, level M, at most version 10; an address too long for that is shown as text only). A
   path that is not on the server (a full URL, `//host`) is refused, so a response cannot send the
   player to another site.
2. The player approves on that page in RomM's web UI, where they may grant fewer scopes.
3. Skiff polls `POST /api/auth/device/token` with the `device_code` every `interval`. RomM answers
   400 with `{"detail": ...}` until then: `authorization_pending`; `slow_down` when polled too soon,
   which adds 5 s to the interval for good (a 429 counts the same); `access_denied` when refused
   (207); `expired_token` when the code ran out or was already used (208). Once approved it
   answers 200 with `access_token`, the `device_id` RomM gave this PSP and the `scopes` granted. A
   token without `platforms.read` and `roms.read` could neither browse nor download, so it is not
   kept (209). A token granted a scope Skiff
   did not ask for is not kept either (204): it would sit on a removable Memory Stick able to do
   more than Skiff needs.
4. Skiff writes `token`, `device_id` and `device_identifier` into `config.ini`'s `[auth]` section
   with the same line-preserving edit as Settings, then saves it crash-safely.

The `device_identifier` is 16 random bytes in hexadecimal, made once (on the PSP from KIRK, the TLS
randomness source) and kept, so RomM sees the same device each time this PSP pairs. The
`device_code` is a credential until it expires and the token is one for good: neither is logged,
and every copy of them (the raw response, cJSON's tree, also when it is refused for junk after the
JSON, the request body) is wiped before its memory is freed. The one copy out of reach is what
cJSON frees itself when a body is not JSON at all. Neither request sends a token: both endpoints are open.

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
- **The log rides on the writes**: the engine calls `after_write` right after each block reaches
  the Memory Stick, and the runner flushes the log then, while Wi-Fi reception is paused anyway.
- **Throughput**: writes go to the Memory Stick in 1 MiB blocks
  (`SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES`), on the download's own thread. While the Memory Stick
  writes, Wi-Fi data stops arriving, and each pause costs about 0.1 s before the transfer is back
  to speed, so fewer, larger writes are faster. On a PSP-1000 at 333 MHz a 64 MiB download ran at
  296 KB/s with 128 KB writes, 386 KB/s with 1 MiB writes, and 310 KB/s with 128 KB writes on a
  separate writer thread: the Memory Stick blocks reception whichever thread writes, so a writer
  thread is not worth its complexity. The network alone delivered about 480 KB/s
  (`tests/hardware/resume_probe.c`, `scenarios=speed`).
- **Storage seam**: the download writes through `include/skiff/storage.h`, not the C library. On the
  PSP newlib's `off_t` is 32 bits, so stdio cannot place a file position past 2 GiB; the PSP
  implementation uses `sceIo` with 64-bit offsets. A rename never replaces a file (FAT cannot do it
  in one step): the target is removed first.
- **Limits**: every download attempt checks free space before its request
  (`skiff_storage_check_room()`): what is still to come plus 8 MiB
  (`SKIFF_STORAGE_FREE_MARGIN_BYTES`) for the `.resume` file, the log, `config.ini` and the file
  system's own entries, so a download never fills the Memory Stick to the last byte. The target's
  folder must exist (`skiff_storage_mkdirs()`), or the attempt fails with 303 before any request. A
  device that cannot report its free space is not refused: a full one still fails the write with
  301. A file
  of 4 GiB or more is refused whatever the space (FAT32, 304). On the PSP free space comes from the
  device's cluster counts (`sceIoDevctl`, `SCE_PR_GETDEV`).
- **Names**: a file name from RomM passes through `skiff_storage_safe_name()` before it becomes part
  of a path (`include/skiff/storage_paths.h`). Characters FAT refuses (`\ / : * ? " < > |`), control
  characters and bytes that are not UTF-8 become `_`, so a name can never add a folder or a device;
  blanks at the start and blanks and dots at the end go (FAT drops trailing dots); a DOS device name
  (`CON`, `NUL`, `COM1`…) gets a leading `_`; a name over 127 bytes is shortened between characters,
  keeping its extension. A name that cleans to nothing (`..`) is refused.
- **The download queue** (`PSP/GAME/Skiff/queue.json`, `include/skiff/jobs.h`): each job is a
  ROM id, its file's name in RomM, the target path the installer chose, the size and CRC-32, a
  state (queued, active, done, failed, cancelled), the last error and the attempt count. It is
  saved on every state change, replaced whole (below). The URL is built when the job runs, so a new
  server address or token applies to jobs already queued. A job left active by a quit or a crash
  is queued again on the next launch and resumes from its `.part` file; a damaged queue file is not
  used (the queue starts empty, and the log says so), and an unusable job in it is dropped alone.
  The same file of the same ROM is never queued twice. At most 64 jobs: finished ones make room.
  When a download finishes, the runner hands the job to the app (`skiff_jobs_config.downloaded`,
  on the worker thread, with neither queue lock held) before it saves the job as done, so the
  installer records the file in `installed.json` before the UI can show the job finished; the
  record takes the logical path (`games:/Game.iso`) back from the job's real one
  (`skiff_storage_logical_path()`). A record that cannot be saved leaves the file counted as copied
  by hand, which is never overwritten.
- **Small files replaced whole** (`config.ini`, the download queue, `installed.json`):
  `skiff_storage_replace_whole()`. FAT cannot replace a file in one step, so Skiff writes and syncs
  `<file>.tmp`, renames it `<file>.new` (so a `.new` file is always complete), syncs the device so
  that rename is on it, removes the file, renames `.new` into place and syncs again; on the PSP any
  sync flushes the whole device, directory entries included. `skiff_storage_read_whole()` (and the
  next replacement) first finishes one a power cut interrupted: a `.tmp` file is dropped, a `.new`
  file without the file is put in place, and one beside the file is dropped.
- **Installed state**: `app:/installed.json` (`include/skiff/install.h`) records every file Skiff
  installed: RomM id, RomM file name, logical path (`games:/Game.iso`, so it follows the device),
  size, CRC-32 and when, replaced whole like `config.ini`. At most 512 records and 640 KB (512
  records with the longest names fit, so whatever is recorded can be saved); a file
  Skiff cannot read (cut, edited by hand, written by a newer Skiff) loads as empty, with a warning,
  and the next save first moves it to `installed.json.damaged`, so its records are never destroyed.
  A Memory Stick error while reading it, or too little memory to parse it (checked before parsing,
  since cJSON reports a failed allocation as a parse error), makes the next save refuse rather than
  replace the records with an empty list. At startup records whose file is
  gone (deleted on a computer or in the XMB) or no longer the recorded size (replaced outside
  Skiff) are dropped, so the library shows the game as not installed; a recorded game whose size or CRC-32 RomM now lists differently shows as changed.
  Games copied by hand are not recognised; they are only protected from being overwritten (below).
- **Logical roots**: code addresses `games:` (`<device>/ISO`), `saves:` (`<device>/PSP/SAVEDATA`)
  and `app:` (the EBOOT's folder). The device is the one the EBOOT runs from, read from `argv[0]`:
  `ms0:` for a Memory Stick, `ef0:` for a PSP Go's internal storage. `skiff_storage_resolve()` turns
  `games:/Game.iso` into a real path and refuses `..`, `.`, empty parts, `\` and `:`, so nothing
  resolves outside its root. Host tests point the roots at a temporary directory.
  `skiff_storage_mkdirs()` creates a missing folder and the ones above it (`ISO` on a new Memory
  Stick).

## Covers

`include/skiff/cover.h` (`src/cover/`) turns RomM's small cover into a picture the PSP's GE draws
as it is: 16-bit RGB565 (`GU_PSM_5650`), scaled down with area averaging (each source pixel
weighted by how much of an output pixel it covers, so any ratio keeps the picture's brightness) to
fit a 160×220 box and keeping its shape (a smaller cover keeps its size), transparency drawn over the placeholder grey.
Decoding a 240×320 cover takes about 0.3 s on a PSP-1000 ([hardware findings](hardware-findings.md#covers)), so it runs off the UI thread, and the
decoded picture (about 70 KB) is what the Memory Stick cache will keep.

- **PNG only**, with libpng 1.6.53: pspdev's package on the PSP, the same release built with the
  same default options in the host image. RomM saves every cover it downloads as PNG; artwork
  uploaded in its web UI keeps its own format (JPEG, WebP, GIF, AVIF), which Skiff refuses with
  210 and shows the game without a cover. stb_image, also in the toolchain, was not used: its own
  documentation says it is not meant for untrusted input, and a PSP has no memory protection.
- **Bounded**: a picture over 1024 pixels on a side, or an interlaced one over 1 MB decoded (every
  Adam7 pass touches every row, so it is read whole), is refused (211), as is anything cut short:
  the chunks after the picture are read too. Rows are otherwise streamed through the scaler, so a
  cover never exists whole at full size. libpng's state lives on the heap, not the calling
  thread's stack, and its errors come back as codes, never on stderr or as an abort.
- **Cached on the Memory Stick** in 64 slot files, `app:/covers/<rom_id % 64>.cov`: about 4.5 MB
  at most by construction, with no index to keep and no folder to list, and no text from RomM in a
  file name. A slot names the cover it holds (ROM id, CRC-32 of the server address and of the cover
  path with RomM's `?ts=`), so another ROM sharing the slot, another server or a changed cover is a
  miss and is overwritten; a header with the box size and a CRC-32 over the whole file turns a cut
  or edited file into a miss too, never garbage on screen. A hit is one read of about 68 KB. Writes
  are one write without a sync (a lost cover is fetched again), and whatever a slot file grows by
  needs 16 MB free beyond the 8 MB margin, so covers never take the last of the Memory Stick from
  downloads.
- **Shown on the details screen**, in a 160×220 box on the right with the text wrapped beside it
  from the first frame, so nothing moves when the cover arrives. Once the game's details are in
  (they decide whether it downloads), the browsing thread reads its slot, or fetches the cover from
  RomM, decodes and caches it; the app swaps it in. A cover is best effort: missing, in another
  format, damaged or lost with the connection, the box keeps its placeholder and `skiff.log` gets a
  line, never an error screen. Back stops a cover still loading, and a lost connection is only
  joined again for a request that needs it, never for a cover.
- **Tested** with PNGs written in memory by libpng's writer (every colour type and bit depth,
  interlaced, transparent, cut, and thousands of corrupted copies, some with their CRCs repaired so
  the damage reaches the decoder) under ASan and UBSan; the self-test decodes a 2×2 PNG on the PSP.

## Installers (the platform plugin seam)

An installer maps a RomM platform to where its games go:

```c
typedef struct skiff_installer {
    const char *romm_platform_slug;     /* "psp" */
    const char *const *extensions;      /* {".iso", ".cso", ".zso", NULL} */
    const char *target_dir;             /* "games:", which is <device>/ISO */
    skiff_err (*post_install)(const char *installed_path); /* optional */
} skiff_installer;
```

The first release registers one installer, PSP: `psp` → `.iso`, `.cso`, `.zso` (any case) →
`games:`, which is `<device>/ISO` (`src/install/install.c`). PS1 and emulated systems are new
table entries; see [Adding a platform](adding-a-platform.md). A ROM that is a folder of several
files, a file with another extension, or a name RomM gives in a way Skiff cannot use
(`skiff_romm_name_status`) is listed but not installable, with the reason the UI shows.

**Never replacing a file Skiff did not install.** A download's last step renames its `.part` file
over the target, so `skiff_install_plan_download()` picks a target that can only hold Skiff's own
earlier copy of the same ROM file:

1. Skiff's recorded copy keeps its place (the download replaces it), as long as it is a file
   directly in the installer's folder (a record pointing anywhere else, such as a hand-edited one
   naming `config.ini`, is never a target) and still the size Skiff recorded; one of another size was replaced outside Skiff and is not
   touched. The CRC-32 is not checked, which would mean reading a whole game.
2. Otherwise the file's safe name in the installer's folder, unless a file is there that Skiff did
   not install for this ROM file (copied by hand, or another ROM's file whose name cleans to the
   same safe name, `a/b.iso` and `a:b.iso`, or differs only in case, which FAT ignores), another
   record holds the name, or a queued download is promised it.
3. Otherwise the name with the RomM id before its extension, `Game [1234].iso`, under the same
   checks (shortened between characters when long).
4. Otherwise the download is refused (`SKIFF_ERR_STORAGE_NAME_TAKEN`, 305): nothing is
   overwritten.

A plan is also refused when the manifest could not record the result (full, or a RomM id it
cannot store), so no download ends as a file Skiff no longer knows it owns.

A name whose `.part` or `.resume` file already exists counts as taken too: the download engine
would trust or delete those, and nobody queued them.

The plan is checked again when the download finishes: a job may replace the file at its target
only when the plan found Skiff's own copy there (`replaces_own` and its recorded size, kept in the
queue as the job's `replace_target` and `replace_size`), and only while the file there is still
that size. Otherwise a file that appeared at the target after planning (copied over USB between two
launches, say) stops the download with 305 instead of being removed, and the finished `.part` file
is kept, so a retry only renames it once the player moves the other file.

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
  rename fails, open file handles stop working as after a suspend, a folder cannot be created, or
  the device has a given amount of free space (`tests/support/fake_storage.h`). `storage/` roots
  point at a temporary directory on the host.
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

The `transport` interface (`include/skiff/transport.h`) takes a method (GET, or POST with a small
body sent whole, for pairing), a URL, headers, `Range`/`If-Range` (GET only) and a body callback, and fills a small response record: status, ETag, Content-Length and
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

Server certificates are checked against one CA file. The release ships Mozilla's root
certificates as curl publishes them (`cacert.pem` next to the EBOOT, about 190 KB), and
the app uses it while `config.ini`'s `ca_file` is empty (`SKIFF_APP_DEFAULT_CA_FILE`,
`skiff_app_transport_settings_from()`). A set `ca_file` replaces the bundle rather than adding to
it, so a private CA does not widen trust to every public one, and a player who wants both writes
both into one file. The bundle is a dated file pinned by SHA256 in
`docker/ca-bundle/fetch-ca-bundle.sh`, fetched into the toolchain image ([Toolchain](toolchain.md#the-ca-bundle)).
Skiff's Mbed TLS profile parses all 121 roots of the 2026-09-25 bundle (host build). The full list
is kept until the hardware tier measures its parse time and heap on a PSP; it is trimmed only if
that is slow.

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
- **Mozilla's root certificates (MPL-2.0)** ship as `cacert.pem`, unmodified and so in their source
  form; `scripts/collect-licenses.sh` adds the MPL-2.0 text as `third-party-licenses/cacert/`.
- **argosy-sigil (MPL-2.0)**, once adopted, requires its source to stay available, which the
  pinned public submodule does.
