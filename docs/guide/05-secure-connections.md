# 6. Secure connections and client certificates (mTLS)

> [!NOTE]
> **Coming in the first release.** The server-side setup below works today. You can prepare it now.

This page is optional. Read it if your RomM server uses HTTPS, especially if it is reachable from
the internet.

## Levels of protection

| Setup | Who can read your traffic | Who can reach RomM | Effort |
|---|---|---|---|
| HTTP on an isolated Wi-Fi network | Devices on that network | Devices on that network | None |
| HTTPS with a public certificate (e.g. Let's Encrypt) | Nobody | Anyone with a login or token | Low |
| HTTPS with your own certificate authority | Nobody | Anyone with a login or token | Medium |
| **HTTPS + client certificate (mTLS)** | Nobody | **Only devices holding a certificate you issued** | Medium |

With **mTLS** (mutual TLS), the PSP proves its identity with a certificate before RomM even sees
the request. Even if someone steals the RomM token, they cannot use it without the certificate
and its key.

> [!TIP]
> If your reverse proxy lets you choose cipher suites, keep **ChaCha20-Poly1305** enabled. The PSP
> downloads about twice as fast with it as with AES-GCM. The default settings of Caddy, Traefik
> and nginx already allow it.

## HTTPS with a public certificate

Nothing to do on the PSP. Skiff ships a list of trusted certificate authorities and checks the
server's certificate against it.

## HTTPS with your own certificate authority

Copy your CA certificate (PEM format, the text file starting with `-----BEGIN CERTIFICATE-----`) to
`PSP/GAME/Skiff/ca.pem` and set:

```ini
[server]
url = https://romm.home.arpa
ca_file = ca.pem
```

## mTLS

RomM itself does not check client certificates. The **reverse proxy** in front of it (Caddy,
Traefik, nginx…) does. The steps are: create a small certificate authority, issue a certificate for
the PSP, tell the proxy to require it, and copy the certificate to the PSP.

> [!TIP]
> Requiring client certificates on your main RomM address also blocks your browser, unless you
> install a certificate there too. A common pattern is a **second address just for devices**, e.g.
> `romm-devices.example.com`, that requires certificates, while the main address keeps working
> normally.

### 1. Create the certificates (on your computer)

Use **ECDSA P-256** keys, not RSA: they are much faster for the PSP's 333 MHz processor. On a
PSP-1000 each new secure connection takes about 0.7 s with an ECDSA key and 2 s with an RSA key.

```bash
# A certificate authority that only signs device certificates. Keep skiff-ca.key private.
openssl ecparam -name prime256v1 -genkey -noout -out skiff-ca.key
openssl req -x509 -new -key skiff-ca.key -sha256 -days 3650 \
  -subj "/CN=Skiff device CA" -out skiff-ca.crt

# One certificate per PSP. Name it after the device so you can revoke it individually.
openssl ecparam -name prime256v1 -genkey -noout -out psp-fat.key
openssl req -new -key psp-fat.key -subj "/CN=psp-fat" -out psp-fat.csr
openssl x509 -req -in psp-fat.csr -CA skiff-ca.crt -CAkey skiff-ca.key -CAcreateserial \
  -days 825 -sha256 -extfile <(printf "extendedKeyUsage=clientAuth") -out psp-fat.crt
```

### 2. Require the certificate on the proxy

Give the proxy `skiff-ca.crt` (the CA certificate, **not** its key).

**Caddy**

```caddyfile
romm-devices.example.com {
    tls {
        client_auth {
            mode require_and_verify
            trust_pool file /etc/caddy/skiff-ca.crt
        }
    }
    reverse_proxy romm:8080
}
```

**nginx**

```nginx
server {
    listen 443 ssl;
    server_name romm-devices.example.com;
    # ssl_certificate / ssl_certificate_key for the server as usual
    ssl_client_certificate /etc/nginx/skiff-ca.crt;
    ssl_verify_client on;
    location / {
        proxy_pass http://romm:8080;
    }
}
```

**Traefik** (dynamic configuration)

```yaml
tls:
  options:
    skiff-mtls:
      clientAuth:
        caFiles: [/etc/traefik/skiff-ca.crt]
        clientAuthType: RequireAndVerifyClientCert
```

Then attach it to the RomM router with `tls.options=skiff-mtls@file`.

✅ **Check from your computer:**

```bash
# Without the certificate: the proxy must refuse the connection.
curl -sS https://romm-devices.example.com/api/heartbeat
# With it: RomM answers with JSON.
curl -sS --cert psp-fat.crt --key psp-fat.key https://romm-devices.example.com/api/heartbeat
```

### 3. Copy the certificate to the PSP

Copy `psp-fat.crt` and `psp-fat.key` to `PSP/GAME/Skiff/` and set:

```ini
[server]
url = https://romm-devices.example.com

[mtls]
cert_file = psp-fat.crt
key_file = psp-fat.key
```

### Revoking a PSP

If a PSP or its Memory Stick is lost: delete its RomM token, and stop trusting its certificate.
The simplest way with a small setup is to create a new CA, re-issue certificates for your other
devices, and replace `skiff-ca.crt` on the proxy.
