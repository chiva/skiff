# 4. Connect to RomM

> [!NOTE]
> Connecting needs Skiff **0.2.0** or newer. Skiff 0.1.0 only starts and cannot connect yet.

## Prepare the RomM server

- RomM **5.3 or newer** (shown under your profile → About in the RomM web UI).
- The PSP must be able to reach the server's address. If RomM is only on your home network, the PSP
  must be on a network that can reach it (see [Wi-Fi](02-wifi.md#extra-safety-optional-for-people-with-a-capable-router)).
- A RomM user that can see your PSP games. A dedicated user with read access only (plus save
  uploads, once save sync arrives) is the safest choice.
- Your games under RomM's **PSP** platform, each one a single `.iso`, `.cso` or `.zso` file. Skiff
  lists the others but cannot download them: a game stored as a folder of several files, or in
  another format, says why on its details screen.

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
   asks for: reading your platforms and ROMs, to browse and download, and nothing else. Approve it
   with both ticked: Skiff cannot work without them (error 209). Your favourites need nothing
   more. When save sync arrives, Skiff will ask you to pair again for the extra permissions it
   needs, and RomM's collections will come with that pairing.
3. Skiff checks every few seconds, picks up the approval and shows your library.

The code works only once, and only for the time the PSP counts down (10 minutes with RomM's
defaults). If it runs out, Skiff says "The code has expired"; if the request is denied in RomM, it
says so. Either way, choose **New code** and open the new address.

Skiff stores the resulting token, and the device id RomM gave this PSP, in
`PSP/GAME/Skiff/config.ini`. To revoke this PSP's access later, delete its token in RomM; nothing
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
- From Skiff 0.3.0, a game with cover art in RomM shows it beside its details, a moment after the
  text (0.5 to 0.8 seconds the first time). Skiff keeps the covers you have seen on the Memory
  Stick (`PSP/GAME/Skiff/covers/`, at most about 4.5 MB), so they come back at once; delete that
  folder to free the space. Covers come from RomM's own copy, which is a PNG for every cover RomM
  downloads; artwork you uploaded to RomM as a JPEG or WebP shows as a grey box. The guide's
  [troubleshooting](troubleshooting.md) codes 210 and 211 explain covers that do not show.
- From Skiff 0.4.0, **SELECT** switches the library between all your PSP games and your
  **Favourites**: the games you marked as favourites in RomM's web app (Skiff cannot mark them
  itself). The list you see is not remembered: Skiff starts on all games.
- In Favourites, **START** offers **Download all**. Skiff checks every favourite first (the header
  counts them), then asks, listing what it leaves out: games already installed or already in
  Downloads, games RomM has changed since you installed them (open each to replace it), games it
  cannot install, and games that do not fit: Downloads holds 64 unfinished downloads, Skiff keeps
  track of 512 installed games, and the downloads must fit the Memory Stick's free space. It takes
  games in name order until one does not fit. Press **Back** while it checks to stop.
- **Downloads** shows each download's progress, speed and time left. Downloads carry on while you
  browse. A failed download can be retried or cancelled, and **Clear finished** tidies the list.
- Games go to the `ISO/` folder of the Memory Stick. Play them from Game → Memory Stick, like any
  game you copied there yourself.
- Quitting Skiff (HOME, then quit) pauses the downloads; the next start carries on where they
  stopped. After sleep mode or a Wi-Fi drop, Skiff reconnects by itself.

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
