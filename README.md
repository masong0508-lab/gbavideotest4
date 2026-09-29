# The Three Machinima Mind (Director's Cut) - GBA, stereo.

100 minutes of video at 120x68 / 5 fps (2x scaled to 240x136, Mode 4) with stereo 4-bit ADPCM
audio (~9078.5 Hz per channel: left = FIFO A, right = FIFO B).


## Controls
START pause/resume - A+Right fast-forward (4x) - A+Left rewind. A part loops when it ends.

## Build
- No toolchain: push to GitHub; the Actions workflow builds every part and uploads `build/*.gba`.
- Local: install devkitPro/devkitARM, then `make` -> `build/part01.gba` ...

## Re-encode from the source video
Needs python3, Pillow, numpy, ffmpeg, gcc:

    python3 tools/gbatool.py film input.webm --out parts

## Tests (PC)
    gcc -O2 -o /tmp/t host/test_adpcm.c -lm
    /tmp/t audio.bin audio_state.bin left.raw right.raw
Decodes with the same `adpcm.h` the GBA uses; checks seek states are bit-exact and reports SNR.

## Not yet verified
The player (`main.c`) has not been compiled or run on an emulator/hardware by the author: no ARM
toolchain was available. The shared decoder, encoder, RLE and asset layout are tested on a PC.
