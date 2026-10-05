# Skiff's PSP toolchain: pspdev plus current, pinned TLS libraries. pspdev's own curl (7.64.1, 2019)
# and mbedtls (2.28, out of support since the end of 2024) are removed first, so no stale header or
# archive can be picked up by mistake. libzip goes too: it is the only other package that depends on
# pspdev's mbedtls, and Skiff does not use it.
#
# Every input is pinned: the base image by digest, each archive by SHA256 (in
# toolchain/build-tls.sh, which the host image runs too, so host tests use the same TLS code).
# Nothing is installed from a package repository, so the same commit always builds with the same
# tools; everything the build runs (cmake, make, tar, bzip2, sed) comes from the pinned base image.
FROM pspdev/pspdev:v20261001@sha256:54895e6f5afb71b8f4f6915ee6d5023e1e087ca7afdf2dcb1d7a694a5233e45f

# The docs and AGENTS.md promise GCC 15; fail here if a pspdev bump changes the major version, so
# the hardware tier is re-run before anything is built with a different compiler.
RUN psp-gcc -dumpversion | grep -q '^15\.' \
 && psp-pacman -R --noconfirm curl libzip mbedtls

COPY toolchain/ /opt/skiff-build/
RUN /opt/skiff-build/build-tls.sh "${PSPDEV}/psp" -DCMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake"

WORKDIR /src
