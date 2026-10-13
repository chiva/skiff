# 4. Connect to RomM

> [!NOTE]
> Connecting needs Skiff **0.2.0** or newer. Skiff 0.1.0 only starts and cannot connect yet.

## Prepare the RomM server

- RomM **5.3 or newer** (shown under your profile → About in the RomM web UI). Syncing saves, and
  getting games sent from RomM's web UI, need 5.4 or newer.
- The PSP must be able to reach the server's address. If RomM is only on your home network, the PSP
  must be on a network that can reach it (see [Wi-Fi](02-wifi.md#extra-safety-optional-for-people-with-a-capable-router)).
- A RomM user that can see your PSP games. A dedicated user that can read the library and keep
  its own saves (RomM's default Viewer group) is the safest choice.
- Your games under RomM's **PSP** platform, each one a single `.iso`, `.cso` or `.zso` file, or a
  `.chd` file when RomM converts downloads ([below](#games-stored-as-chd)). Skiff lists the others
  but cannot download them: a game stored as a folder of several files, or in another format, says
  why on its details screen.

## First start

The buttons each screen uses are shown at its bottom. Which of ✕ and ○ confirms follows the PSP's
own setting.

1. **The server address.** Skiff asks for your RomM address. Choose **Edit** and type it on the
   on-screen keyboard, as you would in a browser: for example `https://romm.example.com`, or
   `http://192.168.1.20:8080` (the keyboard starts with `https://` filled in; delete it for a plain
   HTTP address).
2. **The Wi-Fi connection.** Skiff opens the PSP's list of network connections. Pick the one you set
   up in [Wi-Fi](02-wifi.md). Skiff remembers it and joins it by itself from then on. If joining
   fails, the error screen offers **Choose network** to pick another. With the Wi-Fi switch off,
   Skiff waits and asks you to turn it on.
3. **Pairing** (below) starts on its own.

## Pair the PSP

You never type a password or a long token on the PSP. Skiff shows **Pair with RomM**: a QR code, the
address of RomM's pairing page with this PSP's code in it, for example
`https://romm.example.com/pair/device?user_code=7EGGP3VE`, and below it the code and the time left.

1. Scan the QR code with your phone's camera, or type the address exactly on your phone or
   computer: RomM's page has no box to enter the code, it reads it from the address. (An address
   too long for a QR code is shown as text only.)
2. Sign in to RomM and check that the page shows the same code as the PSP. RomM lists what Skiff
   asks for, all ticked:
   - reading your platforms and ROMs, to browse and download. Skiff cannot work without them
     (error 209). Your favourites need nothing more;
   - reading your collections, for Skiff's **Collections** list (below);
   - reading and writing your saves, and RomM's record of this PSP (the device permissions), for
     [save sync](06-sync-saves.md) (Skiff 0.4.0 or newer). Writing RomM's record of this PSP
     also lets it receive [games sent from RomM](#games-sent-from-romm) (Skiff 0.5.0 or newer).

   You may untick all but the first: Skiff still browses and downloads, and the features that need
   the others ask you to pair again when you want them.

   None of them lets Skiff change your library.
3. Skiff checks every few seconds, picks up the approval and shows your library.

The code works only once, and only for the time the PSP counts down (10 minutes with RomM's
defaults). If it runs out, Skiff says "The code has expired"; if the request is denied in RomM, it
says so. Either way, choose **New code** and open the new address.

Skiff stores the resulting token, the device id RomM gave this PSP and the permissions you
granted in `PSP/GAME/Skiff/config.ini`. To revoke this PSP's access later, delete its token in RomM; nothing
else is affected. To pair again (for example after error 200), open **Settings** and choose
**Pair again**.

### Changing the server

In **Settings**, choose the **Server** line and type the new address. Skiff then forgets the old
server's token and pairs with the new one. If downloads are queued, or Skiff installed games from
the old server, it asks first: the downloads are cancelled, and Skiff forgets which games it
installed. The games themselves stay on the Memory Stick.

## Download games

- The **Library** lists the PSP games in RomM. Games Skiff already installed say **Installed**;
  **Changed in RomM** means RomM now has a different file for that game.
- Select a game to see its size and the free space on the Memory Stick, then choose **Download**.
  For a game already installed, Skiff asks before replacing your copy.
- From Skiff 0.6.0, a game's details also show what RomM knows about it, when it knows it: the
  year, genres, number of players, rating (RomM's average of its metadata providers' scores, out
  of 100), developer and publisher, regions and languages, and under a line its description. A
  long description scrolls with **Up** and **Down** (**L** and **R** a screen at a time); arrows
  beside it show there is more, and **Scroll** appears among the buttons. A game RomM has not
  identified with a metadata provider shows only its name, file, size and state, as before. Skiff
  writes curly quotes, dashes and "…" as plain `'`, `"`, `-` and `...`, and shows at most about 2
  KB of a description (its end then reads `...`). The PSP's font has no Japanese, Chinese or Korean
  characters: they show as placeholders.
- From Skiff 0.3.0, a game with cover art in RomM shows it beside its details, a moment after the
  text (0.5 to 0.8 seconds the first time). Skiff keeps the covers you have seen on the Memory
  Stick (`PSP/GAME/Skiff/covers/`, at most about 4.5 MB), so they come back at once; delete that
  folder to free the space. Covers come from RomM's own copy, which is a PNG for every cover RomM
  downloads; artwork you uploaded to RomM as a JPEG or WebP shows as a grey box. The guide's
  [troubleshooting](troubleshooting.md) codes 210 and 211 explain covers that do not show.
- From Skiff 0.4.0, **SELECT** moves the library from all your PSP games to your **Favourites**,
  then to your **Collections**, then back to all games. Favourites are the games you marked as
  favourites in RomM's web app (Skiff cannot mark them itself). The list you see is not
  remembered: Skiff starts on all games.
- **Collections** lists the collections you made in RomM, regular and smart, and other users'
  public ones, each with the number of games RomM counts in it (every platform's). Open one to see
  its PSP games, with the same details as the library; **Back** returns to the collections, and
  **Back** again, or **SELECT**, to all games. You make and fill collections in RomM's web app.
  Seeing them needs the collections permission from pairing: a PSP paired by an older Skiff, with
  that permission unticked, or with a token pasted by hand says **Pair again to see your
  collections**; choose **Pair again** (the address, the downloads and your installed games stay).
  A RomM with very many or very large collections may be too much for the PSP's memory (error
  214, see [troubleshooting](troubleshooting.md)).
- In Favourites, or in an open collection, **START** offers **Download all**. Skiff checks every
  game of the list first (the header counts them), then asks, listing what it leaves out: games already installed or already in
  Downloads, games RomM has changed since you installed them (open each to replace it), games it
  cannot install, and games that do not fit: Downloads holds 64 unfinished downloads, Skiff keeps
  track of 512 installed games, and the downloads must fit the Memory Stick's free space. It takes
  games in name order until one does not fit. Press **Back** while it checks to stop.
- **Downloads** shows each download's progress, speed and time left. Downloads carry on while you
  browse. A failed download can be retried or cancelled, and **Clear finished** tidies the list.
- While games download, the screen turns off after a minute without a button press, to save
  battery; the downloads carry on. Any button lights it again, and that press does nothing else,
  so you never confirm something you could not see. Once the downloads are done, and when you
  quit, your own Backlight Auto-Off setting is back. If you set it to Off, Skiff leaves it alone.
- Games go to the `ISO/` folder of the Memory Stick. Play them from Game → Memory Stick, like any
  game you copied there yourself.
- Quitting Skiff (HOME, then quit) pauses the downloads; the next start carries on where they
  stopped. After sleep mode or a Wi-Fi drop, Skiff reconnects by itself.

### Games stored as CHD

The PSP cannot run a `.chd` file. From Skiff 0.7.0, RomM 5.4 can convert one into a `.cso` as it
sends it, if the server's admin turns that on (it is off by default):

- set `ROM_CONVERTO_ENABLED=true` in the RomM container's environment (RomM's image includes the
  converter, [rom-converto](https://github.com/DevYukine/rom-converto));
- and allow converting downloads in RomM's settings (`converto.download_conversion_enabled: true`
  in `config.yml`).

Then:

- A CHD game's details say **Downloads as CSO: RomM converts it first**. Skiff asks for a CSO, the
  smallest download over the PSP's Wi-Fi, or a ZSO or ISO when RomM offers only those. Where RomM
  does not convert downloads, the details say so instead, and the game cannot be downloaded.
- **Downloads** shows **RomM is converting it; asking again in 30 s** while RomM converts the game:
  a whole UMD takes RomM a few minutes, during which the PSP stays awake. You can cancel it as any
  download. RomM keeps the converted copy for a while (a day by default), so downloading it again
  soon starts at once.
- The game goes to `ISO/` as a `.cso` with the CHD's name, and shows as installed; when RomM gets a
  different CHD for it, it shows **Changed in RomM** as any game does.
- A converted download that is interrupted starts over from the beginning: RomM 5.4.0 serves the
  converted copy as a new version every time, so Skiff cannot safely continue it. Keep the PSP
  charging and near the router for these.
- **Download all** and games sent from RomM take CHD games when RomM says it converts downloads.
  Their size is only known once RomM has converted them, so Skiff counts each one as a quarter
  larger than its CHD when it checks the free space, and checks again when the download starts.

## Games sent from RomM

From Skiff 0.5.0, with RomM 5.4 or newer, you can pick a game in RomM's web UI, on your phone or
computer, and send it to the PSP: Skiff adds it to **Downloads** by itself, without asking.

1. In RomM's web UI, signed in as the RomM user that approved this PSP's pairing, open a PSP game
   and choose **Install on device**.
2. Tick **Skiff on PSP**. RomM lists only your own devices that can receive games; a dot shows the
   PSP online while Skiff is open and connected.
3. With Skiff open, the game is in Downloads within about 10 seconds, also while Skiff sits on
   the Downloads screen.
   Skiff's header says "From RomM: 1 added to Downloads" ("Desde RomM: 1 añadidos a Descargas" on a
   PSP set to Spanish), and RomM says **Added to downloads** and sends you a notification.

Skiff only receives games while it is open. A game you send while Skiff is closed waits in RomM
("Waiting for the device") for up to 2 days, RomM's default, and Skiff adds it right after it
connects the next time you open it. Unticking the PSP in RomM before then cancels the send. Skiff
never joins the Wi-Fi only to look for games: after a Wi-Fi drop it looks again once it has
joined for something else, such as a download carrying on or the next game you open.

Skiff applies the same rules as **Download all**. A game already in Downloads counts as added; an
installed game is reported to RomM as already installed. RomM shows **Failed** with Skiff's reason,
in the PSP's language, for a game Skiff did not add, and Skiff's header then says "From RomM: 1
added, 1 not added (see RomM)":

- a game of another platform: RomM offers Install on device for games of any platform, but Skiff
  only installs PSP games (error 219);
- a game Skiff cannot install: a folder of several files, not an `.iso`, `.cso` or `.zso` file, or
  a `.chd` file when RomM does not convert downloads;
- a game Skiff installed that RomM has changed since: open it in Skiff to replace your copy;
- a game whose download failed in Downloads: retry it there;
- no room: Downloads holds 64 unfinished downloads, Skiff keeps track of 512 installed games, and
  the download must fit the Memory Stick's free space. Send it again once there is room.

**Settings → Games from RomM** turns this off and on (it is on at first). Off, Skiff asks RomM to
stop listing this PSP in Install on device, and adds nothing; your choice is kept in `config.ini`
as `[downloads] from_romm = off`. The row may also say:

- **Pair again to allow it**: the PSP was paired without the device permissions (by a Skiff older
  than 0.4.0, with them unticked, or with a token pasted by hand), or RomM refused it (error 218). Choose the row to pair
  again with the device permissions ticked; the address, the downloads and your installed games
  stay.
- **Needs RomM 5.4**: update RomM.
- **Turned off in RomM**: the RomM server's administrator turned Install on device off.

When the PSP does not show up in Install on device, see
[troubleshooting](troubleshooting.md#the-psp-is-not-listed-in-install-on-device).

## Alternative: paste a token by hand

If you would rather not pair (for example to manage tokens yourself), create a **client API
token** in RomM, then edit `PSP/GAME/Skiff/config.ini` on your computer. With a token there, Skiff
skips pairing:

```ini
[server]
url = http://192.168.1.20:8080

[auth]
token = rmm_...
```

> [!WARNING]
> Anyone who gets your Memory Stick can read this token. Use a token made only for this PSP so you
> can revoke it without affecting anything else.

## Custom headers (Cloudflare Access and similar)

If your RomM sits behind a proxy that expects specific HTTP headers, add them under `[headers]` in
`config.ini`. The common case is a Cloudflare Access service token:

```ini
[headers]
CF-Access-Client-Id = <id>.access
CF-Access-Client-Secret = <secret>
```

> [!WARNING]
> These values are secrets, like the token. Anyone with the Memory Stick can read them.

## HTTP or HTTPS?

- **Plain HTTP on your home network** works, but anyone on that Wi-Fi network can see the token.
  Acceptable on a dedicated, isolated network; not recommended otherwise.
- **HTTPS** is recommended, especially if RomM is reachable from the internet. See
  [Secure connections](05-secure-connections.md).

Next: [Secure connections and mTLS](05-secure-connections.md)
