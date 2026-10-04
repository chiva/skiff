# Debugging on a real PSP

PSPLINK turns a USB-connected PSP into a development target: the computer serves files to the PSP,
you load and restart builds without touching the Memory Stick, read `printf` output live, and
attach `psp-gdb`.

## What you need

- A PSP running custom firmware (ARK-5) and a USB data cable.
- **Native** pspdev on the computer. Docker on macOS cannot reach USB devices, so the host-side
  tools (`usbhostfs_pc`, `pspsh`, `psp-gdb`) must run outside containers. Download the archive for
  your OS from [pspdev releases](https://github.com/pspdev/pspdev/releases), unpack it, and set:

  ```bash
  export PSPDEV="$HOME/pspdev"
  export PATH="$PSPDEV/bin:$PATH"
  ```

  On macOS, clear the download quarantine flag once: `xattr -dr com.apple.quarantine "$PSPDEV"`.
- On Linux, allow your user to access the PSP's USB device:
  `sudo cp "$PSPDEV/share/psplinkusb/50-psplink.rules" /etc/udev/rules.d/ && sudo udevadm control --reload`.

## One-time PSP setup

1. Download `psplink.zip` from [psplinkusb releases](https://github.com/pspdev/psplinkusb/releases).
2. Copy its `psplink` folder to `PSP/GAME/` on the Memory Stick.
3. Start it from Game → Memory Stick → PSPLINK. The screen shows a PSPLINK banner and waits.

## Session

Three terminals, from the repository root:

```bash
# 1. Serve the repo to the PSP as host0:/ (leave running)
usbhostfs_pc "$PWD"

# 2. Interactive shell on the PSP (leave running)
pspsh

# 3. Build
scripts/dev.sh psp
```

In `pspsh`:

```text
host0:/> ./build/psp/skiff_selftest.prx    # run the self-test; output appears here
host0:/> ./build/psp/skiff.prx             # run the app
host0:/> reset                              # back to PSPLINK after a crash or exit
```

✅ The self-test run prints `SKIFF SELFTEST OK n/n`.

## Breakpoints with psp-gdb

The `psp` preset builds PRX modules (relocatable, which PSPLINK needs) and does not strip them.
Load the module stopped at its entry point, then attach gdb to the ELF with symbols:

```text
host0:/> debug ./build/psp/skiff.prx
```

```bash
psp-gdb build/psp/skiff
(gdb) target remote :10001
(gdb) break main
(gdb) continue
```

## Network debugging

To see what the PSP sends, capture on the RomM host or the proxy:

```bash
sudo tcpdump -i any -w psp.pcap host <psp-ip>
```

Open `psp.pcap` in Wireshark. For TLS, check the ClientHello shows TLS 1.2 and an ECDHE cipher
suite, and, with mTLS, that the PSP sends a Certificate message.

## Common problems

| Problem | Fix |
|---|---|
| `usbhostfs_pc` says "could not open device" | Linux: udev rule above. macOS: unplug, replug, restart PSPLINK |
| `pspsh` connects but `host0:/` is empty | `usbhostfs_pc` was started from a different directory |
| PSP freezes on `reset` | Hold the power switch to turn it off; restart PSPLINK |
| macOS refuses to run the binaries | Clear the quarantine flag (see above) |
