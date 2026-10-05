# Host toolchain for unit tests (gcc and clang), sanitizers, coverage, static analysis, icon
# rendering (librsvg2-bin, for scripts/render-icons.sh), and the integration server's certificates
# and checks (openssl, curl, jq; tests/integration/). The CI host jobs run scripts/dev.sh, which
# runs this image, so local and CI results come from the same environment. Not used for PSP builds:
# those run in the skiff-toolchain image (toolchain.Dockerfile).
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates clang cmake cppcheck clang-tidy curl gcc gcovr git jq libclang-rt-18-dev \
      librsvg2-bin make ninja-build openssl \
 && rm -rf /var/lib/apt/lists/*

# The checkout is bind-mounted and owned by the host user, not the container's root. On Linux hosts
# (CI) git then refuses to read it ("dubious ownership"); Docker Desktop hides this by remapping.
RUN git config --system --add safe.directory /src

WORKDIR /src
