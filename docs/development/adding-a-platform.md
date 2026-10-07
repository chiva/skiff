# Adding a platform

> [!NOTE]
> The installer table lives in `src/install/install.c` (`include/skiff/install.h`); PSP is its only
> entry so far. Platform requests are collected with the
> [new platform issue form](https://github.com/chiva/skiff/issues/new?template=new_platform.yml).

Skiff installs a game by downloading it from RomM and putting it where the PSP software that plays
it will look. Supporting a new system means describing that once.

## What a platform needs

| Field | Example (PSP) | Example (NES, hypothetical) |
|---|---|---|
| RomM platform slug | `psp` | `nes` |
| Accepted file extensions | `.iso .cso .zso` | `.nes .zip` |
| Target folder | `games:` (`ms0:/ISO`) | the emulator's ROM folder, e.g. `ms0:/PSP/GAME/<emulator>/ROMS` |
| Post-install step | none | none |
| Saves location | `ms0:/PSP/SAVEDATA/<game id>*` | emulator-specific |

The PSP Go stores games on its internal `ef0:` as well as on `ms0:`; installers use logical paths
and the storage layer resolves the device.

## Rules

- **Placement only.** Skiff does not convert, patch or bundle emulators or BIOS files. If a format
  needs converting (e.g. PS1 `.bin/.cue` to `EBOOT.PBP`), that is a separate, explicit step with its
  own design discussion.
- **One emulator per platform by default**, chosen for compatibility on a PSP-1000; others can be
  offered as a setting.
- **Documented.** Each platform gets a row in the user guide naming the emulator and where to get
  it.
