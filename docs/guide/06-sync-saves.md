# 6. Sync saves

> [!NOTE]
> Save sync needs Skiff **0.4.0** or newer and RomM **5.4** or newer. It has not been through its
> test on a real PSP yet: copy `PSP/SAVEDATA/` to your computer before you rely on it.

Skiff keeps each game's save the same on this PSP and in RomM. It uploads a save you made on the
PSP, and downloads one made elsewhere: on another PSP, or in PPSSPP through a RomM client such as
Argosy ([FAQ](faq.md#do-my-saves-work-with-ppsspp)). A save that changed on one side only syncs on
its own. Skiff asks only when it cannot safely choose which save to keep: both sides hold different
saves and both changed since the last sync (or there was no sync yet, or since pairing again), or a
save was deleted on this PSP or emptied in RomM and still needs a decision. Choosing **Leave it**
for a save deleted on this PSP ends that question; a later change to RomM's save is then downloaded
on its own.

## What it needs

- **RomM 5.4 or newer**, with the PSP platform scanned by it. RomM 5.4 reads each game's save ID
  from its `.iso` or `.cso` file when it scans; a game scanned by an older RomM has none until you
  rescan the platform. With RomM 5.3, Settings → Saves says "Save sync needs RomM 5.4 or newer"
  (error 215); browsing and downloading work as before.
- **The save permissions from pairing**: reading and writing your saves, and RomM's record of this
  PSP ([Connect to RomM](04-connect-to-romm.md#pair-the-psp)). A PSP paired by an older Skiff, with
  those unticked, or with a token pasted by hand says "Saves: pair again to sync them" in the header
  when it starts. Open Settings → **Saves** and choose **Pair again**: the address, the downloads
  and your installed games stay. Your RomM user's role must allow saves (RomM's default Viewer group
  does).
- **A game RomM has a save ID for.** Skiff knows which game in RomM a save on the PSP belongs to
  when Skiff installed that game, or when RomM already holds a save for exactly one game with that
  ID. For any other game, open it in the library and choose **Sync save** once.

## When Skiff starts

Skiff and a game never run at the same time, so saves sync when Skiff starts: play, quit the game,
then open Skiff.

Once the library shows, the header says "Checking saves...". Skiff then does by itself everything
that has only one safe answer: it uploads a save that changed only on this PSP, and downloads one
that changed only in RomM, or that only RomM has (also for a game not on this PSP yet, so the save
is there when you install it). Before a download replaces or removes a save on this PSP, Skiff
keeps a backup of it (below). Then the header says, for example, "Saves: 2 synced, 1 need you".
Downloads of games carry on meanwhile.

The games that need you wait in Settings → **Saves**. Until you choose, the library's Settings
button counts them ("Settings (1)"), and Settings' **Saves** row says how many need you.

## Settings → Saves

The list holds every game with a save on this PSP or in RomM (up to 128), with its state. Games
that need you come first, their state in amber; the rest follow by name:

| State | What it means | Sync |
|---|---|---|
| Synced 11 Oct | The same save on both sides, or a save deleted on this PSP that you chose to leave | Says the save is already in sync |
| Changed on this PSP | You played since the last sync | Uploads it |
| Newer in RomM | Another device saved since the last sync | Downloads it, after a backup |
| Only in RomM | This PSP has no save for the game | Downloads it |
| Changed on both | Both sides saved since the last sync | Asks (below) |
| Differs from RomM's | Both have a save and this PSP never synced the game, or paired again since | Asks |
| Deleted on this PSP | You deleted the save on the PSP after a sync | Asks |
| Deleted in RomM | RomM's newest save for the game is empty: it was deleted on another device | Asks |
| Can't sync | See [What is not synced](#what-is-not-synced) | Says why |

Under the list, the selected game's state in full and when it last synced ("Last synced 11 Oct
18:15"). The buttons each screen uses are shown at its bottom:

- **Sync** compares the selected game again, reading its save afresh, and uploads, downloads or
  asks. On a game that can't sync, it says why.
- **Sync all** compares every game again, as at launch, also asking RomM again, a few at a time,
  about installed games it had no save ID for.
- **Backups** lists the selected game's backups.

A game's details in the library show its state too ("Save: Newer in RomM"), and offer **Sync save**
when RomM has a save ID for the game.

## Which save? (when Skiff asks)

The header names the game. The screen says in one line what happened ("Changed on both since the
last sync", or "The two saves differ and have never synced"), then when each side last changed, in
columns:

```
This PSP   11 Oct   18:15
RomM       11 Oct   18:19   Argosy on Pixel
```

with the device RomM's save came from when RomM says (or the emulator that uploaded it). The PSP's
date is its own clock; RomM's is the time RomM recorded, often UTC, so the two can differ by your
time zone. Under them, each button with what it does (the footer names them too):

- **Keep this PSP's** uploads this PSP's save. RomM keeps the one it replaces in its history.
- **Use RomM's** downloads RomM's save. This PSP's is backed up first (see Backups).
- **Later** does nothing; the game still needs you.

For a save deleted in RomM ("Deleted in RomM since the last sync"; RomM's side says Deleted):
**Keep this PSP's** uploads it again; **Remove this PSP's** deletes it on this PSP too, after a
backup.

For a save deleted on this PSP ("Deleted on this PSP since the last sync"): **Download again** puts
RomM's back; **Leave it** keeps this PSP without the save while RomM keeps its copy. Skiff then
stops asking, until RomM's save changes: that one it downloads, since there is nothing on this PSP
to lose.

## Backups

Before it replaces or removes a game's saves on this PSP, Skiff zips them into
`PSP/GAME/Skiff/saves-backup/`. It keeps the three newest backups of each game, and drops the
oldest of all while the backups add up to more than 32 MB (never the one just made). Deleting the
`PSP/GAME/Skiff` folder deletes them too.

To put a backup back: Settings → **Saves**, select the game, choose **Backups**, pick a backup (each
shows when it was made) and choose **Restore**. Skiff asks "Restore this backup? The saves on this
PSP are backed up first." and says "Backup restored". Skiff then compares the game again, as after
you play: a restored save that differs from the one last synced counts as changed on this PSP, and
the next sync uploads it (or asks, if RomM's changed too).

## Pairing again, or a new server

Pairing again, perhaps as another RomM user, makes Skiff forget what it last synced. At the next
check, a game with the same save on both sides is simply recorded, and one with different saves
says "Differs from RomM's" and asks once. Changing the server forgets the same way. Your saves and
backups stay.

## If something interrupts a sync

When the Wi-Fi, sleep mode or a flat battery cuts a sync short, an upload leaves no partial save in
RomM (one RomM had already received whole is found the same on both sides at the next check), and a
download changes nothing on the PSP until it has arrived whole and matches RomM's checksum (error
217 otherwise). After sleep mode Skiff joins the Wi-Fi again before its next request; a sync you
started that sleep mode cut short runs once more by itself once the Wi-Fi is back, and if sleep
mode cuts that one too, Skiff says the connection was lost (111). A restore that sleep mode cut is
settled first, as soon as the Memory Stick answers again (Skiff tries for a few seconds); if it
still cannot be, the Saves screen says why and ✕ Retry tries again. A restore cut while it was replacing the game's folders is finished the next time
Skiff starts, which says "A save restore was cut short; Skiff has now finished it" (308). If Skiff's
copy of the save it was restoring, or its note of the restore, is gone, cut short or unreadable, it
stops with error 309 and leaves the folders as they are; the backup it made first holds the save as
it was. If the Memory Stick cannot be read or written meanwhile, save sync waits until a later start
finishes the restore. [Troubleshooting](troubleshooting.md) explains errors 212 to 217 and 306 to 309.

## Saves you delete in RomM

Skiff never deletes a save in RomM, and deleting one in RomM's web app does not delete it on the
PSP. With no save left in RomM for the game, the next sync uploads this PSP's again. With an older
save left, Skiff takes that one as RomM's newer save: if this PSP's has not changed since the last
sync, it downloads it, keeping a backup of this PSP's first; otherwise it asks.

## What is not synced

A game that cannot sync shows **Can't sync**, with the reason under the list:

- **"Can't sync: RomM has no save ID for this game. With RomM 5.4 or newer, rescan the
  platform."** A game scanned before RomM 5.4, a game stored as a `.pbp`, or a file RomM cannot
  read as a disc. The game's details say "Save: no save ID in RomM".
- **"Can't sync: Skiff does not know which game in RomM uses this save. Open the game in the
  library and choose Sync save."** Save folders on this PSP for a game Skiff did not install and
  RomM holds no save for yet; the row shows the game's ID, such as `ULUS10064`. A game that is not
  in RomM at all cannot sync.
- **"Several games in RomM use this save. Open the one you play in the library and choose Sync
  save."** RomM has two copies of the game, for example an `.iso` and a `.cso`, and keeps separate
  saves for each.
- **"Can't sync:" and an error**, for example 307: a save folder holds something a PSP save never
  does. [Troubleshooting](troubleshooting.md) says what to do; **Sync** tries again.

Never synced, and left alone:

- game data that some games install into `PSP/SAVEDATA/`, which is not a save;
- saves RomM keeps for the game in another slot, or without a slot (Skiff uses the `autosave`
  slot only);
- PS1 saves, until Skiff installs PS1 games;
- RomM's in-browser player, which uploads no PSP saves (a game's own save data) to RomM.
