# 5. Connect to RomM

> [!NOTE]
> **Coming in the first release.** This page describes how connecting will work so you can prepare
> your server. The current build cannot connect yet.

## Prepare the RomM server

- RomM **5.3 or newer** (shown under your profile → About in the RomM web UI).
- The PSP must be able to reach the server's address. If RomM is only on your home network, the PSP
  must be on a network that can reach it (see [Wi-Fi](03-wifi.md#extra-safety-optional-for-people-with-a-capable-router)).
- A RomM user that can see your PSP games. A dedicated user with read access only (plus save
  uploads, once save sync arrives) is the safest choice.

## Pair the PSP (recommended)

You never type a password or a long token on the PSP:

1. In Skiff, open **Settings → Server** and enter the RomM address, e.g. `http://192.168.1.20:8080`.
   The on-screen keyboard is used once for this.
2. Choose **Pair with RomM**. Skiff shows a short code.
3. On your phone or computer, open RomM, go to the device approval page Skiff names, and enter the
   code.
4. Skiff picks up the approval within a few seconds and shows your library.

Skiff stores the resulting token in `PSP/GAME/Skiff/config.ini`. To revoke this PSP's access
later, delete its token in RomM; nothing else is affected.

## Alternative: paste a token by hand

If pairing is not possible (for example an older RomM), create a **client API token** in RomM,
then edit `PSP/GAME/Skiff/config.ini` on your computer:

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
  [Secure connections](06-secure-connections.md).

Next: [Secure connections and mTLS](06-secure-connections.md)
