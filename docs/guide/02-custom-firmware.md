# 2. Custom firmware

## What it is, and why you need it

The PSP's own software (Sony's "official firmware") only runs games from UMD discs and Sony's store.
**Custom firmware** (CFW) adds the ability to run homebrew apps like Skiff and games stored as files
on the Memory Stick. It does not remove anything you have.

We recommend **ARK-5**, installed permanently with **CustomIPL**, on top of official firmware
**6.61**:

- **ARK-5** is the actively maintained successor of ARK-4 (ARK-4 is archived). It runs PSP games
  from ISO and compressed CSO/ZSO files, and PS1 games.
- **CustomIPL** makes it survive turning the PSP off. Without it, you would have to re-launch the
  custom firmware every time the PSP boots.
- **6.61** is the last official firmware, and the one ARK-5 is built for.

> [!IMPORTANT]
> The installation steps change between ARK releases, so this guide does not copy them. Follow the
> **official instructions** linked below, and use this page to understand what you are doing and
> to check you have done it right.

## Before you start

- [ ] Battery above 75% **and** the charger plugged in.
- [ ] Memory Stick formatted on the PSP, with at least 100 MB free.
- [ ] You know your firmware version: Settings → System Settings → System Information → System
      Software.

## Step 1: Update to official firmware 6.61

If your PSP already shows **6.61**, skip this step.

Use Sony's 6.61 update file, copied to `PSP/GAME/UPDATE/EBOOT.PBP` on the Memory Stick, then run it
from Game → Memory Stick. Do not turn the PSP off while it updates.

✅ **Check:** System Information shows `6.61`.

## Step 2: Install ARK-5

Follow the instructions in the [ARK-5 installer (FasterARK)](https://github.com/PSP-Arkfive/FasterARK)
repository. In short, you copy the installer to the Memory Stick, run it from the Game menu, and it
launches ARK.

✅ **Check:** System Information now mentions ARK, and the Game menu shows a "Recovery" or ARK
menu entry.

## Step 3: Make it permanent with CustomIPL

Run the CustomIPL installer included with ARK-5, following the same official instructions.

On a **PSP-1000**, ARK-5 also bundles an emergency recovery tool (DC10). It is worth reading about
in the ARK documentation: it can revive a PSP if something ever goes badly wrong.

✅ **Check:** turn the PSP fully off (hold the power switch up for 3 seconds), turn it on, and
System Information still mentions ARK.

## Where games go

| What | Folder on the Memory Stick |
|---|---|
| PSP games (`.iso`, `.cso`, `.zso`) | `ISO/` |
| Homebrew apps and PS1 games (`EBOOT.PBP`) | `PSP/GAME/<name>/` |
| Saves | `PSP/SAVEDATA/` |

Skiff puts games in these folders for you.

Next: [Wi-Fi](03-wifi.md)
