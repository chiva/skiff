#!/usr/bin/env bash
# The sigil cross-check: Skiff's save units against argosy-sigil's, on one synthetic memory stick
# (make_tree.py), in the host image (scripts/dev.sh sigil-check). Fails unless:
#   1. Skiff and sigil collect the same folders into the same unit, byte for byte, with the same
#      content_hash, which is also what RomM's own algorithm gives;
#   2. sigil restores Skiff's unit to an empty stick with every file of the game's save folders,
#      and nothing of the install or the other game;
#   3. Skiff reads and extracts sigil's unit with the same hash and files.
# Usage: tests/sigil/check.sh <work dir> <skiff_unit_tool> <sigil_driver>
set -euo pipefail

readonly USAGE="usage: tests/sigil/check.sh <work dir> <skiff_unit_tool> <sigil_driver>"
readonly WORK="${1:?$USAGE}"
readonly SKIFF_TOOL="${2:?$USAGE}"
readonly SIGIL_DRIVER="${3:?$USAGE}"
readonly GAME="SKIF00001"
readonly GAME_FOLDERS=("${GAME}DATA00" "${GAME}DATA01" "${GAME}NOSFO" "${GAME}ORDER")
readonly LEFT_OUT=("${GAME}INSTALL" "SKIF00002DATA00")
readonly APP="PSP/GAME/Skiff"
readonly SAVEDATA="PSP/SAVEDATA"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly HERE

fail() {
  echo "SIGIL CHECK FAIL: $*" >&2
  exit 1
}

# RomM 5.4.0's hash_zip_contents (backend/handler/filesystem/assets_handler.py), verbatim in effect.
romm_hash() {
  python3 -I -c '
import hashlib, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    lines = [n + ":" + hashlib.md5(z.read(n)).hexdigest() for n in sorted(z.namelist()) if not n.endswith("/")]
print(hashlib.md5("\n".join(lines).encode()).hexdigest())' "$1"
}

# same_folders <stick> <other root holding the folders>
same_folders() {
  local folder
  for folder in "${GAME_FOLDERS[@]}"; do
    diff -r "$1/$SAVEDATA/$folder" "$2/$folder" >/dev/null || fail "$folder differs under $2"
  done
  for folder in "${LEFT_OUT[@]}"; do
    [[ ! -e "$2/$folder" ]] || fail "$folder should have been left out, but is under $2"
  done
}

rm -rf "$WORK/stick" "$WORK/restored" "$WORK/reader"
mkdir -p "$WORK/stick" "$WORK/restored" "$WORK/reader/$APP"
python3 -I "$HERE/make_tree.py" "$WORK/stick"

skiff_hash="$("$SKIFF_TOOL" write "$WORK/stick" "$GAME" skiff.zip)"
sigil_hash="$("$SIGIL_DRIVER" collect "$WORK/stick" "$GAME" "$WORK/sigil.zip")"
echo "content_hash: Skiff $skiff_hash, sigil $sigil_hash"
[[ "$skiff_hash" == "$sigil_hash" ]] || fail "the content hashes differ"
cmp "$WORK/stick/$APP/skiff.zip" "$WORK/sigil.zip" || fail "the units differ"
echo "1. the same unit, byte for byte ($(wc -c <"$WORK/sigil.zip") bytes)"
[[ "$(romm_hash "$WORK/sigil.zip")" == "$skiff_hash" ]] || fail "RomM's algorithm gives another hash"
echo "   RomM's algorithm agrees"

"$SIGIL_DRIVER" restore "$WORK/restored" "$GAME" "$WORK/stick/$APP/skiff.zip" >/dev/null
same_folders "$WORK/stick" "$WORK/restored/$SAVEDATA"
echo "2. sigil restored Skiff's unit: every file of ${#GAME_FOLDERS[@]} folders, no install"

cp "$WORK/sigil.zip" "$WORK/reader/$APP/sigil.zip"
read_hash="$("$SKIFF_TOOL" read "$WORK/reader" "$GAME" sigil.zip stage)"
[[ "$read_hash" == "$sigil_hash" ]] || fail "Skiff read sigil's unit with hash $read_hash"
same_folders "$WORK/stick" "$WORK/reader/$APP/stage"
echo "3. Skiff read and extracted sigil's unit: same hash, same files"
echo "SIGIL CHECK OK"
