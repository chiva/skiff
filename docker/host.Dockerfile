# Host toolchain for unit tests (gcc and clang), sanitizers, coverage, static analysis, icon
# rendering (librsvg2-bin, for scripts/render-icons.sh), and the integration server's certificates
# and checks (openssl, curl, jq; tests/integration/), the launch check's disc image (genisoimage,
# below), reading QR codes back (zbar-tools, below), zlib for the downloads' CRC-32 (the PSP
# links pspdev's zlib), cJSON for RomM's responses and libpng for covers (both built below at
# pspdev's versions). The CI host jobs run scripts/dev.sh, which
# runs this image, so local and CI results come from the same environment. Not used for PSP builds:
# those run in the skiff-toolchain image (toolchain.Dockerfile).
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bzip2 ca-certificates clang cmake cppcheck clang-tidy curl gcc gcovr git jq \
      libclang-rt-18-dev librsvg2-bin make ninja-build openssl zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

# The same TLS stack the EBOOTs link (curl over Mbed TLS with Skiff's profile, pinned by SHA256),
# built by the toolchain image's script, so host tests exercise the PSP's TLS code and error codes
# rather than the distribution's libcurl. find_package() finds it through CMAKE_PREFIX_PATH.
ENV CMAKE_PREFIX_PATH=/opt/skiff-tls
COPY toolchain/ /opt/skiff-build/
RUN /opt/skiff-build/build-tls.sh /opt/skiff-tls

# cJSON for RomM's responses (src/romm/), the version pspdev packages for the PSP (1.7.16), so host
# tests parse with the same code the EBOOTs link. Static only; pinned by SHA256.
ARG CJSON_VERSION=1.7.16
ARG CJSON_SHA256=451131a92c55efc5457276807fc0c4c2c2707c9ee96ef90c47d68852d5384c6c
RUN curl -fsSL --retry 3 -o /tmp/cjson.tar.gz \
      "https://github.com/DaveGamble/cJSON/archive/refs/tags/v${CJSON_VERSION}.tar.gz" \
 && echo "${CJSON_SHA256}  /tmp/cjson.tar.gz" | sha256sum -c - \
 && mkdir /tmp/cjson && tar -xzf /tmp/cjson.tar.gz -C /tmp/cjson --strip-components=1 \
 && cmake -S /tmp/cjson -B /tmp/cjson/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DENABLE_CJSON_TEST=OFF -DENABLE_CJSON_UTILS=OFF -DENABLE_CUSTOM_COMPILER_FLAGS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/skiff-cjson \
 && cmake --build /tmp/cjson/build --target install \
 && rm -rf /tmp/cjson /tmp/cjson.tar.gz
ENV CMAKE_PREFIX_PATH=/opt/skiff-tls:/opt/skiff-cjson

# libpng for covers (src/cover/), the version pspdev packages for the PSP (1.6.53), built with
# libpng's default options as pspdev's is, so host tests decode with the same code. Static only;
# pinned by SHA256.
ARG LIBPNG_VERSION=1.6.53
ARG LIBPNG_SHA256=b20cee717e11416d2f96ccc7d184f63730ca8cb2f03bfd0c4ed77fbc909c0bff
RUN curl -fsSL --retry 3 -o /tmp/libpng.tar.gz \
      "https://github.com/pnggroup/libpng/archive/refs/tags/v${LIBPNG_VERSION}.tar.gz" \
 && echo "${LIBPNG_SHA256}  /tmp/libpng.tar.gz" | sha256sum -c - \
 && mkdir /tmp/libpng && tar -xzf /tmp/libpng.tar.gz -C /tmp/libpng --strip-components=1 \
 && cmake -S /tmp/libpng -B /tmp/libpng/build -DCMAKE_BUILD_TYPE=Release -DPNG_SHARED=OFF \
      -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/skiff-png \
 && cmake --build /tmp/libpng/build --target install \
 && rm -rf /tmp/libpng /tmp/libpng.tar.gz
ENV CMAKE_PREFIX_PATH=/opt/skiff-tls:/opt/skiff-cjson:/opt/skiff-png

# The QR Code generator the PSP links (src/ui/qr.c), from the same pinned script.
COPY qrcodegen/ /opt/skiff-qrcodegen-build/
RUN /opt/skiff-qrcodegen-build/build-qrcodegen.sh /opt/skiff-qrcodegen
ENV CMAKE_PREFIX_PATH=/opt/skiff-tls:/opt/skiff-cjson:/opt/skiff-png:/opt/skiff-qrcodegen

# genisoimage writes the launch check's UMD disc image (tests/hardware/make_launch_disc.py, for the
# hardware tier); zbarimg reads back the QR codes the unit tests draw (tests/unit/test_ui_qr.c). A
# layer of its own, after the TLS build, so adding to it rebuilds nothing above.
RUN apt-get update \
 && apt-get install -y --no-install-recommends genisoimage zbar-tools \
 && rm -rf /var/lib/apt/lists/*

# The checkout is bind-mounted and owned by the host user, not the container's root. On Linux hosts
# (CI) git then refuses to read it ("dubious ownership"); Docker Desktop hides this by remapping.
RUN git config --system --add safe.directory /src

WORKDIR /src
