#!/bin/sh
# Verifies that the split asset pieces in parts/<name>/ still join back into the original streams
# (run this after cloning / uploading to GitHub). Uses parts/<name>/SHA256SUMS written when the files were split.
# usage: tools/verify_assets.sh [parts/film]
d="${1:-parts/film}"
want() { sed -n "$1p" "$d/SHA256SUMS" | cut -d' ' -f1; }
ok=0
chk() { got=$(cat "$@" | sha256sum | cut -d' ' -f1); [ "$got" = "$(want $n)" ] && echo "OK   $*" || { echo "FAIL $*"; ok=1; }; }
n=1; chk "$d/frames1a.bin" "$d/frames1b.bin"
n=2; chk "$d/audio_a.bin" "$d/audio_b.bin"
n=3; chk "$d/frames2.bin"
exit $ok
