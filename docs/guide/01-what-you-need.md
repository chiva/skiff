# 1. What you need

## The PSP

Any model works: PSP-1000 ("Fat"), 2000 ("Slim"), 3000 ("Brite"), Go, or Street (E1000).

- **Custom firmware, already installed.** Skiff is homebrew, so the PSP must run custom firmware.
  Skiff is tested on **ARK-5** with official firmware 6.61. This guide does not cover installing it:
  follow your custom firmware's own instructions (for ARK-5,
  [FasterARK](https://github.com/PSP-Arkfive/FasterARK)). Connecting to RomM needs **ARK** (ARK-4
  or ARK-5) specifically: on other custom firmware Skiff runs but shows error 109 when it connects.
- **Memory Stick.** A Memory Stick Pro Duo, or a microSD card in a Pro Duo adapter (cheap and
  works well). 8 GB or more is comfortable; a PSP game is usually between 200 MB and 1.8 GB. The PSP
  Go has 16 GB built in and uses Memory Stick Micro (M2) instead.
- **A USB cable** (mini-USB on most models; the Go uses its own connector) to copy files from your
  computer. A card reader for the microSD also works.

✅ **Check:** Settings → System Settings → System Information mentions your custom firmware (for
example ARK).

## The network

A Wi-Fi network the PSP can join. The PSP's Wi-Fi is from 2004 and modern routers often refuse it.
[Chapter 2](02-wifi.md) explains what to change; it is usually one setting or a guest network.

## The RomM server

A running [RomM](https://github.com/rommapp/romm) server, version **5.3 or newer**, with your PSP
games scanned into it. You need:

- its address, as you type it in a browser (for example `http://192.168.1.20:8080` or
  `https://romm.example.com`);
- a RomM user that can see your games.

## The games

Skiff downloads games from your own RomM library. It does not provide games. Only use dumps of
games you own.

Next: [Wi-Fi](02-wifi.md)
