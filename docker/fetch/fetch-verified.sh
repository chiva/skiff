# shellcheck shell=sh
# Sourced, not run: downloads pinned by SHA256 for the scripts that build the toolchain and host
# images (toolchain/, ca-bundle/, qrcodegen/ and host.Dockerfile). Both images copy this directory
# to /opt/skiff-docker/fetch/, beside the scripts, so it is found at ../fetch/ in the repository and
# in the images alike.
#
# Transient failures (GitHub's release downloads, curl.se, a runner's DNS: "wget: bad address
# 'curl.se'") have failed CI image builds, so each download gets a few attempts. pspdev's wget is
# BusyBox's, which has no retry option, and curl's --retry skips DNS and connection errors, so the
# loop below retries for both tools. It re-fetches on a checksum mismatch too (a truncated
# download).
readonly FETCH_ATTEMPTS=3
readonly FETCH_RETRY_DELAY_SECONDS=5

# pspdev's image has wget, the host image curl; either is fine because the checksum decides.
fetch() {
  if command -v wget >/dev/null 2>&1; then
    wget -q -O "$2" "$1"
  else
    curl -fsSL -o "$2" "$1"
  fi
}

# fetch_verified <url> <sha256> <file>: downloads <url> to <file> and checks it, or stops the build
# naming the URL and why its last attempt failed.
fetch_verified() {
  attempt=1
  while :; do
    status=0
    fetch "$1" "$3" || status=$?
    if [ "$status" -ne 0 ]; then
      failure="download failed (exit status $status)"
    elif echo "$2  $3" | sha256sum -c -; then
      break
    else
      failure="SHA256 mismatch"
    fi
    if [ "$attempt" -ge "$FETCH_ATTEMPTS" ]; then
      echo "error: $1: $failure, giving up after $FETCH_ATTEMPTS attempts" >&2
      exit 1
    fi
    echo "warning: $1: $failure (attempt $attempt of $FETCH_ATTEMPTS)," \
      "retrying in $FETCH_RETRY_DELAY_SECONDS s" >&2
    attempt=$((attempt + 1))
    sleep "$FETCH_RETRY_DELAY_SECONDS"
  done
}
