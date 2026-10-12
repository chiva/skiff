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

## Do my saves work with PPSSPP?

Skiff keeps each game's save in the game's `autosave` slot in RomM, the slot where RomM clients
such as Argosy keep PPSSPP's saves, so a save one side uploads is offered to the other. On a real
PSP, a save made there loaded in PPSSPP 1.20.4, and one made in PPSSPP loaded on the PSP, with
PPSSPP's `EncryptSave` setting (in `ppsspp.ini`) left on, its default. That was checked with one
game, not every game. See [Sync saves](06-sync-saves.md).

## And with RomM's web player?

No. RomM's in-browser player uploads no PSP saves (a game's own save data) to RomM, so there is
nothing to exchange with it.

## Which emulator does RomM show for Skiff's saves?

`psp_console`, in slot `autosave`, as `<game ID>.zip` with the date RomM adds (for example
`ULUS10064 [2026-10-11_18-15-01].zip`). The label only says where a save came from: RomM matches a
game's saves by slot, whichever emulator made them, which is how a PPSSPP save and this PSP's meet.

## Does Skiff ever delete saves in RomM?

No. **Keep this PSP's** adds a new version, and RomM keeps the one it replaced in its history. Skiff
asks RomM to keep the 10 newest saves labelled `psp_console` for each game (every PSP's), so RomM
drops older ones with that label; saves PPSSPP clients upload under their own label are not
counted. On the PSP, a save is replaced or
removed only by a download from RomM (**Use RomM's** and **Remove this PSP's** among them) or a
backup you restore, and Skiff backs it up first.

## What if I have two PSPs?

Pair each one. RomM records them as separate devices, and each Skiff keeps its own record of what
it synced. A save made on one is downloaded by the other the next time it starts, as long as the
other's has not changed since it last synced; if both changed, the second asks which to keep. RomM
refuses to let a PSP replace a save it has not seen (error 212), so nothing is overwritten.

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
