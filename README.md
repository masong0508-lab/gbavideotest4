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

## Konami clip (ROM part 01 only)
In the first ROM, the Konami code on the main menu (U U D D L R L R B A) fades the menu to black in ~0.13 s and plays
a heavily squashed clip instead of the secret image. Other ROMs keep the secret image.
- Clip assets live in `clip/` (5 files, ~0.41 MiB). `tools/split_parts.py parts/film gen --clip clip` reserves room for them
  and copies them into `part01` only; the Makefile then builds that ROM with `-DHAVE_CLIP`.
- Video: 120x34 stored, doubled vertically by a row-repeat opcode (`run 0`), 16 colours per scene, lossy RLE, 2.5 unique fps.
- Audio: **ADPCM2** = 2-bit mono ADPCM at 4539 Hz (76 bytes per 4-vblank chunk), FIFO A on both speakers; encoder is a beam search.
- Re-encode: `python3 tools/clipenc.py input.mp4 --out clip [--target 380]` (bytes per frame; higher = better/bigger).
- PC test (same `adpcm.h`/`rle.h` as the GBA): `gcc -O2 -o /tmp/tc host/test_clip.c && /tmp/tc clip frames.raw audio.raw`

## Not yet verified
The player (`main.c`) has not been compiled or run on an emulator/hardware by the author (the Konami clip code was only compile-checked on a PC and its decoders unit-tested): no ARM
toolchain was available. The shared decoder, encoder, RLE and asset layout are tested on a PC.
