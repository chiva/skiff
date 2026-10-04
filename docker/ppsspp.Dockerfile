# PPSSPPHeadless built from source at a pinned commit. PPSSPP ships no headless binary, so CI caches
# this image (keyed on this file's hash) and rebuilds it only when the pin changes.
#
# Every input is pinned: the Debian base by digest, PPSSPP (and through it, its submodules) by
# commit, and every Debian package by installing from a dated snapshot.debian.org archive, so the
# same commit always builds the same emulator. Packages arrive over plain HTTP because the base image
# has no CA certificates; apt verifies them against Debian's signing keys, as on any Debian mirror.
# Bump SNAPSHOT alongside PPSSPP_COMMIT (any timestamp from snapshot.debian.org).
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS base

ARG DEBIAN_FRONTEND=noninteractive
ARG SNAPSHOT=20261001T000000Z
# A snapshot's Release files are past their validity date by design.
RUN rm -f /etc/apt/sources.list.d/debian.sources \
 && printf '%s\n' \
      'Types: deb' \
      "URIs: http://snapshot.debian.org/archive/debian/${SNAPSHOT}" \
      'Suites: trixie trixie-updates' \
      'Components: main' \
      'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
      '' \
      'Types: deb' \
      "URIs: http://snapshot.debian.org/archive/debian-security/${SNAPSHOT}" \
      'Suites: trixie-security' \
      'Components: main' \
      'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
      > /etc/apt/sources.list.d/snapshot.sources \
 && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99snapshot

FROM base AS build
ARG PPSSPP_COMMIT=fa50bb1976065c4f8b1b47af227d367fe9771555 # v1.20.4
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libgl1-mesa-dev libglu1-mesa-dev libsdl2-dev \
      ninja-build python3 \
 && rm -rf /var/lib/apt/lists/*

# Shallow fetch of the pinned commit only; the full history is about a gigabyte.
RUN git init -q /ppsspp \
 && git -C /ppsspp fetch -q --depth 1 https://github.com/hrydgard/ppsspp.git "$PPSSPP_COMMIT" \
 && git -C /ppsspp checkout -q FETCH_HEAD \
 && git -C /ppsspp -c submodule.ffmpeg.update=none submodule update --init --recursive --depth 1
# On Linux, PPSSPP serves flash0: from a VFS that accepts only plain reads, but sceIoOpen always adds
# PPSSPP's internal "quiet" flag, so every read of flash0: from a program fails (seen with the
# firmware fonts the UI prototype loads; still unfixed upstream as of v1.20.4). Mask the flag. The
# last grep fails the build if the patch no longer applies, so a PPSSPP bump re-checks it.
RUN f=/ppsspp/Core/FileSystems/DirectoryFileSystem.cpp \
 && sed -i '/^int VFSFileSystem::OpenFile/,/^}/ s/if (access != FILEACCESS_READ) {/if ((access \& ~FILEACCESS_PPSSPP_QUIET) != FILEACCESS_READ) {/' "$f" \
 && grep -qF 'if ((access & ~FILEACCESS_PPSSPP_QUIET) != FILEACCESS_READ) {' "$f"
# The ffmpeg submodule (prebuilt media libraries for every platform) is gigabytes and only serves
# video and audio playback, which the self-test never uses; skip it and build without FFmpeg.
RUN cmake -S /ppsspp -B /ppsspp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON \
      -DUNITTEST=OFF -DUSING_QT_UI=OFF -DUSE_FFMPEG=OFF -DUSE_DISCORD=OFF \
 && cmake --build /ppsspp/build --target PPSSPPHeadless

FROM base
RUN apt-get update \
 && apt-get install -y --no-install-recommends libgl1 libglu1-mesa libsdl2-2.0-0 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /ppsspp/build/PPSSPPHeadless /usr/local/bin/PPSSPPHeadless
COPY --from=build /ppsspp/assets /usr/local/share/ppsspp/assets
WORKDIR /src
