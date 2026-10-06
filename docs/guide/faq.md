# FAQ

## Is this legal?

Skiff is a download manager for your own RomM server. It contains no games and no Sony code.
Custom firmware and homebrew are legal to use in most countries; what matters is where your games
come from. Only use dumps of games you own.

## Does it work on the PS Vita?

Skiff targets real PSP hardware. On a Vita, use [RomM Vita](https://github.com/abduznik/rommvita),
which runs natively and is faster.

## Does it work in PPSSPP?

Skiff runs in PPSSPP, which is how its automated tests work. There is little point using it there:
PPSSPP can open games straight from your computer.

## Why is downloading slower than on my phone?

The PSP's Wi-Fi is 802.11b (11 Mbit/s at best), and its processor has to decrypt everything it
receives over HTTPS. On a PSP-1000 Skiff downloads at about 3.3 Mbit/s (410 KB/s), so a 1 GB game
takes about 45 minutes. Plug in the charger; Skiff resumes a download that was interrupted.

## Will my saves work with PPSSPP or RomM's web player?

That is the plan for save sync: PSP saves are the same format on a real PSP and in PPSSPP, so a
save made on your PSP can continue in RomM's in-browser PPSSPP player and back. PS1 saves need a
conversion step and come later.

## Why can't I install a PS1 game?

PS1 support is planned after the first release. The PSP plays PS1 games packaged as `EBOOT.PBP`,
while most RomM libraries store them as `.bin/.cue` or `.chd`, which need converting first.

## Can it install NES, SNES or Game Boy games?

Not in the first release. The design leaves room for it: each system needs a PSP emulator and the
folder it reads games from. You can
[request a platform](https://github.com/chiva/skiff/issues/new?template=new_platform.yml).

## Why "Skiff"?

A skiff is a small, light boat. Big ships can't reach the shore, so a skiff carries the cargo the
last stretch. Your RomM server is the ship; Skiff carries your games to the PSP.
