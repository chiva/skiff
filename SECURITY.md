# Security policy

## Supported versions

Only the latest release receives security fixes.

## Reporting a vulnerability

**Do not open a public issue for security problems.** Report them privately through
[GitHub Security Advisories](https://github.com/chiva/skiff/security/advisories/new).

Include a description, steps to reproduce, the impact you expect, and a suggested fix if you have
one. You will get a reply within 72 hours, and we will agree on a fix and disclosure timeline
together.

## Scope

In scope:

- How Skiff stores and sends credentials: the RomM token in `config.ini`, client certificates and
  keys for mTLS.
- TLS on the PSP: certificate validation, pinning, and the randomness used for key exchange.
- Writing files to the Memory Stick: path traversal from names RomM returns, or overwriting files
  outside the folders Skiff manages.
- The integrity of released zips (verify with `gh attestation verify`).

Out of scope:

- Weaknesses of the PSP itself. In particular, the PSP-1000 cannot use WPA2 Wi-Fi; the guide
  explains how to isolate it on its own network.
- Vulnerabilities in RomM, which belong in the [RomM repository](https://github.com/rommapp/romm/security).
- Anyone with physical access to the Memory Stick can read the token and keys stored on it. The
  guide recommends a dedicated, revocable RomM client token per device for this reason.

## Known limitation: randomness on the PSP

The PSP SDK's default random source for TLS is predictable: it reseeds a Mersenne Twister with the
current second on every call. Networking is not released yet; when it is, Skiff must supply its own entropy
source before any connection is made and refuse to connect if it cannot gather enough
(`SKIFF_ERR_NET_ENTROPY`). This is a release blocker, not an option, and any finding that bypasses it
is a high-severity report. See [architecture](docs/development/architecture.md#randomness-for-tls).
