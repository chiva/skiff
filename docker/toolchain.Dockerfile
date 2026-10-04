# Skiff's PSP toolchain: pspdev plus current, pinned TLS libraries. pspdev's own curl (7.64.1, 2019)
# and mbedtls (2.28, out of support since the end of 2024) are removed first, so no stale header or
# archive can be picked up by mistake. libzip goes too: it is the only other package that depends on
# pspdev's mbedtls, and Skiff does not use it.
#
# Every archive is pinned by SHA256. When bumping a version, verify the new archive first: mbedtls
# publishes SHA256 sums in its release notes; curl archives are signed by Daniel Stenberg
# (27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2), see docs/development/toolchain.md.
FROM pspdev/pspdev:v20261001@sha256:54895e6f5afb71b8f4f6915ee6d5023e1e087ca7afdf2dcb1d7a694a5233e45f

ARG MBEDTLS_VERSION=4.1.1
ARG MBEDTLS_SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
ARG CURL_VERSION=8.22.0
ARG CURL_SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7

# The docs and AGENTS.md promise GCC 15; fail here if a pspdev bump changes the major version, so
# the hardware tier is re-run before anything is built with a different compiler.
# python3 runs mbedtls's config.py; xz unpacks the curl archive whose signature was verified.
RUN psp-gcc -dumpversion | grep -q '^15\.' \
 && apk add --no-cache python3 xz \
 && psp-pacman -R --noconfirm curl libzip mbedtls

COPY toolchain/configure-mbedtls.sh /usr/local/bin/configure-mbedtls

WORKDIR /build

RUN wget -q -O mbedtls.tar.bz2 \
      "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2" \
 && echo "${MBEDTLS_SHA256}  mbedtls.tar.bz2" | sha256sum -c - \
 && tar -xjf mbedtls.tar.bz2 \
 && configure-mbedtls "mbedtls-${MBEDTLS_VERSION}" \
 && cmake -S "mbedtls-${MBEDTLS_VERSION}" -B mbedtls-build -Wno-dev \
      -DCMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake" \
      -DCMAKE_INSTALL_PREFIX="${PSPDEV}/psp" \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTING=OFF \
      -DENABLE_PROGRAMS=OFF \
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
 && cmake --build mbedtls-build -j "$(nproc)" \
 && cmake --install mbedtls-build \
 && install -D -m 644 "mbedtls-${MBEDTLS_VERSION}/LICENSE" "${PSPDEV}/psp/share/licenses/mbedtls/LICENSE" \
 && install -D -m 644 "mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/LICENSE" \
      "${PSPDEV}/psp/share/licenses/mbedtls/tf-psa-crypto/LICENSE" \
 && rm -rf /build/*

# HTTPS only, IPv4 only, no resolver thread (the PSP resolver is synchronous), and every optional
# dependency off. CA bundle and path are unset because Skiff passes the CA file at runtime.
# zlib is off for now: RomM's JSON pages are small and decompression costs RAM; revisit with
# Phase 1 measurements.
RUN wget -q -O curl.tar.xz "https://curl.se/download/curl-${CURL_VERSION}.tar.xz" \
 && echo "${CURL_SHA256}  curl.tar.xz" | sha256sum -c - \
 && tar -xJf curl.tar.xz \
 && cmake -S "curl-${CURL_VERSION}" -B curl-build -Wno-dev \
      -DCMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake" \
      -DCMAKE_INSTALL_PREFIX="${PSPDEV}/psp" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_STATIC_LIBS=ON \
      -DBUILD_CURL_EXE=OFF \
      -DBUILD_TESTING=OFF \
      -DBUILD_EXAMPLES=OFF \
      -DBUILD_LIBCURL_DOCS=OFF \
      -DBUILD_MISC_DOCS=OFF \
      -DENABLE_CURL_MANUAL=OFF \
      -DCURL_USE_PKGCONFIG=OFF \
      -DHTTP_ONLY=ON \
      -DCURL_USE_MBEDTLS=ON \
      -DCURL_USE_OPENSSL=OFF \
      -DENABLE_IPV6=OFF \
      -DENABLE_THREADED_RESOLVER=OFF \
      -DENABLE_UNIX_SOCKETS=OFF \
      -DCURL_DISABLE_SOCKETPAIR=ON \
      -DUSE_NGHTTP2=OFF \
      -DUSE_LIBIDN2=OFF \
      -DCURL_USE_LIBPSL=OFF \
      -DCURL_USE_LIBSSH2=OFF \
      -DCURL_ZLIB=OFF \
      -DCURL_BROTLI=OFF \
      -DCURL_ZSTD=OFF \
      -DCURL_DISABLE_ALTSVC=ON \
      -DCURL_DISABLE_COOKIES=ON \
      -DCURL_DISABLE_DOH=ON \
      -DCURL_DISABLE_HSTS=ON \
      -DCURL_DISABLE_NETRC=ON \
      -DCURL_DISABLE_WEBSOCKETS=ON \
      -DCURL_DISABLE_AWS=ON \
      -DCURL_DISABLE_KERBEROS_AUTH=ON \
      -DCURL_DISABLE_NEGOTIATE_AUTH=ON \
      -DCURL_CA_BUNDLE=none \
      -DCURL_CA_PATH=none \
 && cmake --build curl-build -j "$(nproc)" \
 && cmake --install curl-build \
 && install -D -m 644 "curl-${CURL_VERSION}/COPYING" "${PSPDEV}/psp/share/licenses/curl/COPYING" \
 && rm -rf /build/*

# Read by the CI toolchain check and by scripts that need to report what they built with.
RUN printf 'mbedtls %s\ncurl %s\n' "${MBEDTLS_VERSION}" "${CURL_VERSION}" \
      > "${PSPDEV}/psp/share/skiff-toolchain-versions"

WORKDIR /src
