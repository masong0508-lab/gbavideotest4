#!/bin/sh
# Checks that every asset piece in parts/<name>/ is present and joins back into the original stream
# (run after uploading to GitHub). Checksums are in parts/<name>/SHA256SUMS, one line per check, in this order.
# usage: sh tools/verify_assets.sh [parts/film]
d="${1:-parts/film}"
bad=0
check() {  # check <line-in-SHA256SUMS> <file>...
    want=$(sed -n "$1p" "$d/SHA256SUMS" | cut -d' ' -f1); shift
    for f in "$@"; do [ -f "$d/$f" ] || { echo "MISSING $d/$f"; bad=1; return; }; done
    got=$(for f in "$@"; do cat "$d/$f"; done | sha256sum | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then echo "OK      $*"; else echo "CORRUPT $*"; bad=1; fi
}
check 1 frames1a.bin frames1b.bin
check 2 audio_a.bin audio_b.bin
check 3 frames2.bin
check 4 frames_idx.bin
check 5 palette.bin
check 6 audio_state.bin
exit $bad
