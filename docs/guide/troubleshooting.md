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

## RomM errors (200–209)

| Code | Meaning | What to try |
|---|---|---|
| 200 | Login rejected | The token was deleted or expired: pair again |
| 201 | Not allowed | Your RomM user lacks permission; check its role in RomM |
| 202 | Not found | The game was removed from RomM; refresh the list |
| 203 | Server error | Check RomM's logs |
| 204 | Unexpected response | Often a proxy login page (e.g. Authelia, Cloudflare Access) in front of RomM. Exempt `/api/` for devices, or use mTLS |
| 205 | Unsupported RomM version | Update RomM to 5.3 or newer |
| 206 | The file does not match RomM's checksum | The whole file arrived, but its CRC32 is not the one RomM recorded, so Skiff deleted it rather than install a damaged game. This usually means the file was replaced on the server after RomM scanned it: rescan the platform in RomM, then download again. If it keeps happening with the same file, the copy on the server may be damaged |
| 207 | Pairing was refused in RomM | Someone chose to deny this PSP on RomM's pairing page. If that was a mistake, start pairing again on the PSP and approve it |
| 208 | The pairing code expired | The code is valid for a few minutes (RomM shows how long) and only once. Start pairing again on the PSP and enter the new code in RomM in time |
| 209 | Pairing was approved without the permissions Skiff needs | On RomM's pairing page, someone unticked reading platforms or ROMs, so the token could neither browse nor download; Skiff did not keep it. Delete the half-approved device in RomM, pair again and leave those permissions ticked |

## Memory Stick errors (300–305)

| Code | Meaning | What to try |
|---|---|---|
| 300 | No Memory Stick | Re-insert it |
| 301 | Not enough space | Delete games you no longer play from `ISO/` |
| 302 | Read/write failure | Back up the card and reformat it on the PSP; cheap microSD adapters sometimes fail |
| 303 | File or folder missing | Usually harmless; Skiff recreates its folders |
| 304 | File larger than 4 GB | The PSP's file system cannot store it; this game cannot be installed as-is |
| 305 | The game's name is taken | Skiff never replaces a file it did not install. **Before the download:** a file with this game's name, and another with the name plus its RomM number (e.g. `Game [1234].iso`), are already in `ISO/`, copied by hand or by another program: move or rename one of them on a computer, then download again. **At the end of a download:** a file appeared under the game's name while it was downloading (for example copied over USB), or Skiff's own earlier copy is no longer the size it recorded: move or rename that one file, then retry the download. Skiff kept what it downloaded, so the retry finishes without downloading again |

## Settings errors (400–402)

These concern the settings file, `PSP/GAME/Skiff/config.ini`. Deleting it always works as a last
resort; you will need to pair again.

| Code | Meaning | What to try |
|---|---|---|
| 400 | The file is damaged | Open it on a computer and go to the line Skiff names; look for a line without `=` or a missing `]`, or delete the file |
| 401 | A required setting is missing | Add the setting Skiff names, e.g. `key_file` next to `cert_file` (see [Connect to RomM](04-connect-to-romm.md)) |
| 402 | A setting has an invalid value | Fix the value Skiff names: an address must start with `http://` or `https://`; a file name such as `ca_file` must be a file in the Skiff folder, without folders; each setting may appear only once |
