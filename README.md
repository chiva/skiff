<h1 align="center">Skiff</h1>

<p align="center">
  <strong>Your <a href="https://github.com/rommapp/romm">RomM</a> library, on your PSP. Browse, download, play.</strong>
</p>

<p align="center">
  <a href="https://github.com/chiva/skiff/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/chiva/skiff/actions/workflows/ci.yml/badge.svg"></a>
  <a href="https://github.com/chiva/skiff/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/chiva/skiff?include_prereleases"></a>
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/github/license/chiva/skiff"></a>
</p>

> [!WARNING]
> **Early development.** The current build is a scaffold: it installs and starts on a PSP, but it
> cannot talk to RomM yet. Follow the [roadmap](docs/development/roadmap.md) or watch releases to
> know when the first usable version ships.

Today, getting a game onto a PSP means downloading it on a computer, plugging in a USB cable,
copying the file to the right folder and unplugging again. Skiff does it from the PSP itself:
pick a game from your RomM library and it lands on the Memory Stick, ready to play.

Skiff is an independent project. It is not made by or affiliated with the RomM team or Sony.

## What it will do

| | First release | Later |
|---|---|---|
| Browse your RomM PSP library with installed / not installed status | ✅ | |
| Download PSP games (ISO, CSO) to `ms0:/ISO/`, resuming broken downloads | ✅ | |
| Pair with RomM by approving a code in the web UI (no typing tokens) | ✅ | |
| HTTPS, your own certificate authority, and client certificates (mTLS) | ✅ | |
| Cover art, favourites, "download all my favourites" | | ✅ |
| PS1 games | | ✅ |
| Sync PSP saves with RomM (works with RomM's web player and PPSSPP) | | ✅ |
| Retro systems through PSP emulators (NES, SNES, GBA…) | | ✅ |

## What you need

- A PSP (any model) running **custom firmware**. We recommend ARK-5 on firmware 6.61.
- A Wi-Fi network the PSP can join. The PSP is old: it needs 2.4 GHz, 802.11b and WPA (not
  WPA2-only or WPA3). The [Wi-Fi guide](docs/guide/03-wifi.md) shows how to set up a safe one.
- A [RomM](https://github.com/rommapp/romm) server, version 5.3 or newer.
- Games you own. Skiff does not provide games.

New to all this? Start with the **[step-by-step guide](docs/guide/README.md)**. It assumes no
technical background.

## Quick start (for people who have done this before)

1. Download `skiff-<version>.zip` from [Releases](https://github.com/chiva/skiff/releases/latest).
2. Unzip it at the root of the Memory Stick. You should get `PSP/GAME/Skiff/EBOOT.PBP`.
3. On the PSP: Game → Memory Stick → Skiff.

## Documentation

- [User guide](docs/guide/README.md): firmware, Wi-Fi, installing, connecting to RomM, secure
  connections, troubleshooting, FAQ.
- [Developer docs](docs/development/README.md): architecture, toolchain, debugging over USB,
  testing, adding a platform, roadmap.

## Contributing

Bug reports, platform requests and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).
Security problems: see [SECURITY.md](SECURITY.md).

## Credits

- [RomM](https://github.com/rommapp/romm), the library server Skiff talks to.
- [pspdev](https://github.com/pspdev), the open-source PSP toolchain.
- [Grout](https://github.com/rommapp/grout) and [RomM Vita](https://github.com/abduznik/rommvita),
  RomM clients for other handhelds that showed the way.
- [pkgi-psp](https://github.com/bucanero/pkgi-psp), which proved HTTPS downloads work on a PSP.

## License

[MIT](LICENSE). "PSP" and "PlayStation" are trademarks of Sony Interactive Entertainment.
