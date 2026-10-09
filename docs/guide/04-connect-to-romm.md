# 5. Connect to RomM

> [!NOTE]
> **Coming in the first release.** This page describes how connecting will work so you can prepare
> your server. The current build cannot connect yet.

## Prepare the RomM server

- RomM **5.3 or newer** (shown under your profile → About in the RomM web UI).
- The PSP must be able to reach the server's address. If RomM is only on your home network, the PSP
  must be on a network that can reach it (see [Wi-Fi](02-wifi.md#extra-safety-optional-for-people-with-a-capable-router)).
- A RomM user that can see your PSP games. A dedicated user with read access only (plus save
  uploads, once save sync arrives) is the safest choice.

## Pair the PSP (recommended)

You never type a password or a long token on the PSP:

1. In Skiff, open **Settings → Server** and enter the RomM address, e.g. `http://192.168.1.20:8080`.
   The on-screen keyboard is used once for this.
2. Choose **Pair with RomM**. Skiff shows an 8-character code of letters and digits, such as
   `7EGGP3VE`, the page to open as a QR code, and the same page as text: your RomM address
   followed by `/pair/device` and the code, e.g.
   `http://192.168.1.20:8080/pair/device?user_code=7EGGP3VE`.
3. Scan the QR code with your phone's camera, or type the address exactly on your phone or computer:
   RomM's page has no box to enter the code, it reads it from the address. Sign in to RomM and check
   that the page shows the same code as the PSP. RomM lists what
   Skiff asks for: reading your platforms and ROMs, to browse and download, and nothing else.
   Approve it with both ticked: Skiff cannot work without them (error 209). When save sync arrives,
   Skiff will ask you to pair again for the extra permissions it needs.
4. Skiff checks every few seconds, picks up the approval and shows your library.

The code is valid for 10 minutes and only once. If it runs out, Skiff shows error 208; if the
pairing is refused in RomM, error 207. Either way, choose **Pair with RomM** again for a new code.

Skiff stores the resulting token, and the device id RomM gave this PSP, in
`PSP/GAME/Skiff/config.ini`. To revoke this PSP's access later, delete its token in RomM; nothing
else is affected.

## Alternative: paste a token by hand

If you would rather not pair (for example to manage tokens yourself), create a **client API
token** in RomM, then edit `PSP/GAME/Skiff/config.ini` on your computer:

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
