# PPSSPPHeadless built from source at a pinned commit. PPSSPP ships no headless binary, so CI caches
# this image (keyed on this file's hash) and rebuilds it only when the pin changes.
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60 AS build

ARG DEBIAN_FRONTEND=noninteractive
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
# The ffmpeg submodule (prebuilt media libraries for every platform) is gigabytes and only serves
# video and audio playback, which the self-test never uses; skip it and build without FFmpeg.
RUN cmake -S /ppsspp -B /ppsspp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON \
      -DUNITTEST=OFF -DUSING_QT_UI=OFF -DUSE_FFMPEG=OFF -DUSE_DISCORD=OFF \
 && cmake --build /ppsspp/build --target PPSSPPHeadless

FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libgl1 libglu1-mesa libsdl2-2.0-0 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /ppsspp/build/PPSSPPHeadless /usr/local/bin/PPSSPPHeadless
COPY --from=build /ppsspp/assets /usr/local/share/ppsspp/assets
WORKDIR /src
