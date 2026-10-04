# Host toolchain for unit tests (gcc and clang), sanitizers, coverage and static analysis. The CI
# host jobs run scripts/dev.sh, which runs this image, so local and CI results come from the same
# environment. Not used for PSP builds: those run in the pspdev/pspdev image.
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates clang cmake cppcheck clang-tidy gcc gcovr git libclang-rt-18-dev make \
      ninja-build \
 && rm -rf /var/lib/apt/lists/*

# The checkout is bind-mounted and owned by the host user, not the container's root. On Linux hosts
# (CI) git then refuses to read it ("dubious ownership"); Docker Desktop hides this by remapping.
RUN git config --system --add safe.directory /src

WORKDIR /src
