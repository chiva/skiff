# Troubleshooting

Skiff shows every error as a sentence plus a number in brackets, e.g.
"Could not reach the RomM server [102]". Find the number below. Include it in bug reports.

## Skiff does not appear in the Game menu

- The folder layout must be exactly `PSP/GAME/Skiff/EBOOT.PBP` at the top of the Memory Stick. A
  common mistake is an extra folder level, like `PSP/GAME/skiff-0.1.0/PSP/GAME/Skiff/`.
- Custom firmware must be running. Check System Information mentions ARK.

## Skiff starts, then the PSP returns to the menu or freezes

Turn the PSP fully off (hold the power switch up for 3 seconds) and try again. If it keeps
happening, [open a bug report](https://github.com/chiva/skiff/issues/new?template=bug_report.yml)
with your PSP model and firmware.

## Not an error (5)

| Code | Meaning | What to try |
|---|---|---|
| 5 | Cancelled | Nothing to fix: the download was cancelled from the queue, and its partial file deleted. Choose the game again to download it from the start |

## Network errors (100–111)

| Code | Meaning | What to try |
|---|---|---|
| 100 | Wi-Fi off or no network profile | Flip the Wi-Fi switch on; set up a connection in Network Settings ([guide](02-wifi.md)) |
| 101 | The server name could not be found | Check the address in Settings; try the server's IP address instead of its name |
| 102 | The server did not answer | Is RomM running? Can the PSP's network reach it? Open the same address on a phone on the same Wi-Fi |
| 103 | The server took too long | Weak Wi-Fi signal; move closer to the router |
| 104 | Secure connection failed | The server may require TLS settings the PSP cannot do (TLS 1.3 only). Allow TLS 1.2 on the proxy |
| 105 | Server certificate not trusted | Self-signed or private CA: set `ca_file` ([guide](05-secure-connections.md#https-with-your-own-certificate-authority)) |
| 106 | Client certificate rejected | Check `cert_file`/`key_file` names, that the certificate is signed by the CA the proxy trusts, and that it has not expired |
| 107 | Not enough randomness for a secure connection | The PSP's hardware random number generator failed a check. Restart Skiff; if it happens again, report a bug with your PSP model and custom firmware version |
| 108 | The PSP's date and time are wrong | Certificates are only valid between two dates, so the PSP needs the right date to check them. Set it in Settings → System Settings → Date & Time Settings. The date often resets after the battery runs completely flat |
| 109 | Networking needs ARK custom firmware | Every connection, even plain HTTP, needs random numbers from the PSP's hardware generator, which Skiff reads through ARK-4 or ARK-5. Other custom firmware runs Skiff but cannot connect. Install ARK following its own instructions (for ARK-5, [FasterARK](https://github.com/PSP-Arkfive/FasterARK)) |
| 110 | Could not join the Wi-Fi network | The PSP has the connection saved but could not join it. Check that the access point is on and in range, and that its settings suit the PSP: 2.4 GHz, 802.11b allowed, WPA/WPA2 mixed rather than WPA2-only ([guide](02-wifi.md)). Test the connection in Settings → Network Settings |
| 111 | The connection to the server was lost | The connection dropped in the middle of a transfer, usually because the Wi-Fi signal faded, the PSP's Wi-Fi switch was turned off or the server restarted. Move closer to the router and try again; a download continues from where it stopped |

## RomM errors (200–219)

| Code | Meaning | What to try |
|---|---|---|
| 200 | Login rejected | The token was deleted or expired: in **Settings**, choose **Pair again** |
| 201 | Not allowed | Your RomM user lacks permission; check its role in RomM |
| 202 | Not found | The game was removed from RomM. Quit and open Skiff again to see the current list |
| 203 | Server error | Check RomM's logs |
| 204 | Unexpected response | Often a proxy login page (e.g. Authelia, Cloudflare Access) in front of RomM. Exempt `/api/` for devices, or use mTLS |
| 205 | Unsupported RomM version | Update RomM to 5.3 or newer |
| 206 | The file does not match RomM's checksum | The whole file arrived, but its CRC32 is not the one RomM recorded, so Skiff deleted it rather than install a damaged game. This usually means the file was replaced on the server after RomM scanned it: rescan the platform in RomM, then download again. If it keeps happening with the same file, the copy on the server may be damaged |
| 207 | Pairing was refused in RomM | Someone chose to deny this PSP on RomM's pairing page. If that was a mistake, start pairing again on the PSP and approve it |
| 208 | The pairing code expired | The code is valid for a few minutes (the PSP counts them down) and only once. On the PSP choose **New code**, then scan the new QR code or open the new address in time |
| 209 | Pairing was approved without the permissions Skiff needs | On RomM's pairing page, someone unticked reading platforms or ROMs, so the token could neither browse nor download; Skiff did not keep it. Delete the half-approved device in RomM, pair again and leave those two permissions ticked (the others, for save sync and collections, may be unticked) |
| 210 | A cover in a format Skiff cannot show | Only in `skiff.log`: the game shows without its cover. Skiff shows PNG covers, which RomM makes for every cover it downloads; artwork uploaded in RomM's web UI as JPEG, WebP, GIF or AVIF keeps its format. Upload a PNG instead |
| 211 | A cover Skiff could not read | Only in `skiff.log`: the game shows without its cover. The picture is damaged, or larger than 1024 pixels on a side (RomM's small covers are a few hundred). Choose another cover for the game in RomM |
| 212 | RomM has a newer save | Another device (a second PSP, or PPSSPP through a client such as Argosy) saved this game in RomM after this PSP last synced it, so RomM kept that save rather than replace it with this PSP's. Nothing was overwritten: Skiff compares the two again by itself, and Settings → **Saves** asks which to keep ([Sync saves](06-sync-saves.md#which-save-when-skiff-asks)) |
| 213 | The save is too large for RomM | RomM, or a proxy in front of it, refused the upload as too large. RomM accepts saves of up to 512 MB unless `MAX_ASSET_UPLOAD_SIZE_BYTES` sets another limit; a reverse proxy may allow far less (nginx: `client_max_body_size`, 1 MB by default). Raise the proxy's limit for `/api/saves` |
| 214 | Too many collections, or too large ones | RomM sends every collection you can see (yours and other users' public ones) in one answer, with every game in it, and that answer is too large for the PSP's memory: about 3,300 games with covers across all of them, or more than 128 collections. Browsing and favourites are not affected. Make large collections of your own smaller (or delete those you no longer use), or ask whoever owns a large public one to make it private; then choose **Retry** |
| 215 | Save sync needs RomM 5.4 or newer | Browsing and downloading work with RomM 5.3, but syncing saves relies on what RomM 5.4 added: a record of what each device synced, and the save ID it reads from each PSP game when it scans. Update RomM to 5.4 or newer, then rescan the PSP platform so RomM reads each game's save ID |
| 216 | This PSP may not sync saves | RomM refused a save sync request with this PSP's token: your RomM user's role lacks the save permissions (reading and writing assets and devices), or the token no longer has them. In **Settings**, choose **Pair again** and leave save sync ticked; your downloads and installed games stay. If it keeps happening, check that your RomM user's role may read and write assets and devices. A PSP paired by an older Skiff, with save sync unticked on RomM's pairing page, or with a token pasted by hand shows no error: Settings → **Saves** offers **Pair again** ([Sync saves](06-sync-saves.md)) |
| 217 | The save from RomM does not match its checksum | The save arrived whole, but its contents are not the ones RomM recorded for it, so Skiff did not restore it: the save on this PSP is untouched. This usually means the save file was changed on the server; sync again. If it keeps happening, upload the save again from the device that made it |
| 218 | RomM did not let this PSP receive games | RomM refused this PSP's token when Skiff registered it to receive games sent from RomM's web UI (**Install on device**) or asked for them: the token lacks the device permissions (writing devices, reading ROMs), or it is not this PSP's own (a token pasted by hand). In **Settings**, choose **Pair again** and leave the device permissions ticked; your downloads and installed games stay. If it keeps happening, check that your RomM user's role may write devices |
| 219 | That game is not a PSP game | Not shown on the PSP: it is the reason RomM shows for a game sent to this PSP from RomM's web UI that belongs to another platform. RomM offers **Install on device** for any game, but Skiff only installs PSP games. Send a game from RomM's PSP platform instead |

## Memory Stick errors (300–309)

| Code | Meaning | What to try |
|---|---|---|
| 300 | No Memory Stick | Re-insert it |
| 301 | Not enough space | Delete games you no longer play from `ISO/` |
| 302 | Read/write failure | Back up the card and reformat it on the PSP; cheap microSD adapters sometimes fail |
| 303 | File or folder missing | Usually harmless; Skiff recreates its folders |
| 304 | File larger than 4 GB | The PSP's file system cannot store it; this game cannot be installed as-is |
| 305 | The game's name is taken | Skiff never replaces a file it did not install. **Before the download:** a file with this game's name, and another with the name plus its RomM number (e.g. `Game [1234].iso`), are already in `ISO/`, copied by hand or by another program: move or rename one of them on a computer, then download again. **At the end of a download:** a file appeared under the game's name while it was downloading (for example copied over USB), or Skiff's own earlier copy is no longer the size it recorded: move or rename that one file, then retry the download. Skiff kept what it downloaded, so the retry finishes without downloading again |
| 306 | A save is damaged or unsafe | Skiff checks every save from RomM before it touches the Memory Stick, and refused this one: a file in it is damaged or cut short, it is not a save of this game, or it holds folders or names a PSP save never has (another program may have uploaded it in a different layout). Nothing on the Memory Stick changed. Upload the save to RomM again from a device where the game loads it |
| 307 | A save folder holds something Skiff cannot sync | One of the game's folders in `PSP/SAVEDATA/` holds what a PSP save never does: a folder inside it, a name with letters other than plain English ones, digits and common punctuation, or a name longer than 64 characters; or the game's saves add up to more than 64 folders, 256 files or 64 MB. Skiff leaves those saves alone and does not sync the game. Look at the game's folders on a computer, and remove what the game did not put there |
| 308 | A save restore was finished at launch | Not an error. Skiff was putting a save from RomM (or one of its backups) on the Memory Stick when it was cut short (the PSP slept, ran out of battery or was switched off), and finished it when it started again: the game's save is now the one Skiff was restoring. The save the game had before is kept as a backup |
| 309 | A save restore could not be finished | Skiff was putting a save on the Memory Stick when it was cut short, and the copy it was restoring from is gone or damaged (for example, Skiff's `saves-work` folder was deleted from a computer), so Skiff left the game's save folders as they were. If the game no longer loads its save, restore the game's newest backup in Skiff, which holds the save as it was before |

## Settings errors (400–402)

These concern the settings file, `PSP/GAME/Skiff/config.ini`. Deleting it always works as a last
resort; you will need to pair again.

| Code | Meaning | What to try |
|---|---|---|
| 400 | The file is damaged | Open it on a computer and go to the line Skiff names; look for a line without `=` or a missing `]`, or delete the file |
| 401 | A required setting is missing | Add the setting Skiff names, e.g. `key_file` next to `cert_file` (see [Copy the certificate to the PSP](05-secure-connections.md#3-copy-the-certificate-to-the-psp)) |
| 402 | A setting has an invalid value | Fix the value Skiff names: an address must start with `http://` or `https://`; a file name such as `ca_file` must be a file in the Skiff folder, without folders; each setting may appear only once. On an HTTPS server with no `ca_file` set, `cacert.pem` is missing from the Skiff folder: unzip the release again |
