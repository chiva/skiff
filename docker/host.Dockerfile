# Host toolchain for unit tests (gcc and clang), sanitizers, coverage, static analysis, icon
# rendering (librsvg2-bin, for scripts/render-icons.sh), and the integration server's certificates
# and checks (openssl, curl, jq; tests/integration/), and zlib for the downloads' CRC-32 (the PSP
# links pspdev's zlib). The CI host jobs run scripts/dev.sh, which
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

# The checkout is bind-mounted and owned by the host user, not the container's root. On Linux hosts
# (CI) git then refuses to read it ("dubious ownership"); Docker Desktop hides this by remapping.
RUN git config --system --add safe.directory /src

WORKDIR /src
