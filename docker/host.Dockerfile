# Host toolchain for unit tests, sanitizers, coverage and static analysis. Mirrors the packages the
# CI host jobs install on ubuntu-24.04, so `scripts/dev.sh test` reproduces CI on any machine that
# has Docker. Not used for PSP builds: those run in the pspdev/pspdev image.
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates cmake cppcheck clang-tidy gcc gcovr git make ninja-build \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
