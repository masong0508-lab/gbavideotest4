#!/usr/bin/env python3
"""
split_parts.py - cut one oversized film part into several ROM-sized parts (pure Python 3, no dependencies).

A GBA cartridge addresses at most 32 MiB, so parts/film (~63 MiB) cannot be one ROM. This tool cuts it on
video-frame boundaries into the fewest parts that fit, keeping audio and picture in sync, and writes
OUT/part01, OUT/part02, ... in exactly the layout main.c and the Makefile expect.

    python3 tools/split_parts.py parts/film gen          # what the GitHub workflow runs
    python3 tools/split_parts.py parts/film parts        # local build: then just `make`

Input pieces (either naming works): frames1a/1b/2.bin or frames1/2.bin, audio_a/b.bin or audio.bin,
frames_idx.bin, palette.bin, audio_state.bin.   Every output part is checked against the input before exit.
"""
import argparse, json, os, struct, sys

FPS_NUM = 5486                 # main.c: video frame = (vblank_tick * 5486) >> 16  (two ticks per unique frame)
CHUNK = 304                    # bytes per audio chunk (152 L + 152 R); one chunk plays for 4 vblanks
VB_PER_CHUNK = 4
ROM_LIMIT = 32 * 1024 * 1024
CODE_ALLOW = 256 * 1024        # generous room for player code, cart header, alignment
REQUIRED = ["frames_idx.bin", "palette.bin", "audio_state.bin"]


def read_first(d, names):
    for group in names:
        if all(os.path.exists(os.path.join(d, n)) for n in group):
            return b"".join(open(os.path.join(d, n), "rb").read() for n in group)
    sys.exit("missing input in %s: need one of %s" % (d, " | ".join("+".join(g) for g in names)))


def t_of(u):                   # vblank tick at which unique frame u starts (each unique frame lasts 2 video frames)
    return 2 * u * 65536 / FPS_NUM


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("out")
    a = ap.parse_args()

    for r in REQUIRED:
        if not os.path.exists(os.path.join(a.src, r)):
            sys.exit("ERROR: %s/%s is missing - upload it to the repository" % (a.src, r))
    data = read_first(a.src, [["frames1a.bin", "frames1b.bin", "frames2.bin"], ["frames1.bin", "frames2.bin"]])
    audio = read_first(a.src, [["audio_a.bin", "audio_b.bin"], ["audio.bin"]])
    idx_raw = open(os.path.join(a.src, "frames_idx.bin"), "rb").read()
    pal_raw = open(os.path.join(a.src, "palette.bin"), "rb").read()
    st_raw = open(os.path.join(a.src, "audio_state.bin"), "rb").read()

    ne = len(idx_raw) // 4
    idx = struct.unpack("<%dI" % ne, idx_raw)
    nu = ne - 1                                             # unique frames
    if idx[-1] != len(data):
        sys.exit("frames_idx.bin says %d bytes of video but the frame files hold %d" % (idx[-1], len(data)))
    segs = [struct.unpack("<I16H", pal_raw[i:i + 36]) for i in range(0, len(pal_raw), 36)]
    nchunks = len(audio) // CHUNK
    if len(audio) % CHUNK or len(st_raw) != nchunks * 8:
        sys.exit("audio.bin / audio_state.bin sizes do not match (%d bytes, %d chunks, %d state bytes)" % (len(audio), nchunks, len(st_raw)))
    states = struct.unpack("<%dI" % (nchunks * 2), st_raw)
    print("input: %d unique frames, %d palette segments, %d audio chunks, %.1f MiB total" %
          (nu, len(segs), nchunks, (len(data) + len(audio) + len(idx_raw) + len(pal_raw) + len(st_raw)) / 2**20))

    def chunk_start(u):                                     # audio chunk that begins at unique frame u
        return min(nchunks, int(round(t_of(u) / VB_PER_CHUNK)))

    def nseg(u0, u1):
        f0, f1 = 2 * u0, 2 * u1
        k = sum(1 for s in segs if f0 <= s[0] < f1)
        return k + (0 if any(s[0] == f0 for s in segs) else 1)

    def audio_range(u0, u1, last):
        c0 = chunk_start(u0)
        c1 = nchunks if last else min(nchunks, chunk_start(u1) + 1)   # +1 chunk overlap so the tail never runs dry
        return c0, c1

    def size(u0, u1, last):
        c0, c1 = audio_range(u0, u1, last)
        return (idx[u1] - idx[u0]) + 4 * (u1 - u0 + 1) + 36 * nseg(u0, u1) + (c1 - c0) * (CHUNK + 8)

    cap = ROM_LIMIT - CODE_ALLOW
    cuts = None
    for n in range(1, 40):
        cuts = [0]; ok = True
        for k in range(n - 1):
            u0 = cuts[-1]
            remaining = size(u0, nu, True)
            share = min(cap, remaining / (n - k))
            lo, hi = u0 + 1, nu - 1                         # largest u1 with size(u0,u1) <= share
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if size(u0, mid, False) <= share: lo = mid
                else: hi = mid - 1
            best = min(range(max(u0 + 1, lo - 40), lo + 1),   # nudge onto an audio chunk boundary (keeps sync exact)
                       key=lambda u: abs(t_of(u) / VB_PER_CHUNK - round(t_of(u) / VB_PER_CHUNK)))
            cuts.append(best)
        cuts.append(nu)
        if all(size(cuts[i], cuts[i + 1], i == n - 1) <= cap for i in range(n)):
            break
    else:
        sys.exit("could not fit the film in ROM-sized parts")
    n = len(cuts) - 1
    print("cutting into %d ROM parts" % n)

    os.makedirs(a.out, exist_ok=True)
    manifest = []
    for i in range(n):
        u0, u1 = cuts[i], cuts[i + 1]; last = i == n - 1
        c0, c1 = audio_range(u0, u1, last)
        d = os.path.join(a.out, "part%02d" % (i + 1)); os.makedirs(d, exist_ok=True)
        vid = data[idx[u0]:idx[u1]]
        li = [x - idx[u0] for x in idx[u0:u1 + 1]]
        f0, f1 = 2 * u0, 2 * u1
        ls = []
        cover = max((s for s in segs if s[0] <= f0), key=lambda s: s[0])
        ls.append((0,) + cover[1:])
        ls += [(s[0] - f0,) + s[1:] for s in segs if f0 < s[0] < f1]
        au = audio[c0 * CHUNK:c1 * CHUNK]
        stl = states[c0 * 2:c1 * 2]
        third = len(vid) // 3
        w = lambda name, b: open(os.path.join(d, name), "wb").write(b)
        w("frames1a.bin", vid[:third]); w("frames1b.bin", vid[third:2 * third]); w("frames2.bin", vid[2 * third:])
        w("frames_idx.bin", struct.pack("<%dI" % len(li), *li))
        w("palette.bin", b"".join(struct.pack("<I16H", *s) for s in ls))
        half = ((c1 - c0) // 2) * CHUNK
        w("audio_a.bin", au[:half]); w("audio_b.bin", au[half:])
        w("audio_state.bin", struct.pack("<%dI" % len(stl), *stl))
        total = sum(os.path.getsize(os.path.join(d, x)) for x in os.listdir(d))
        assert total + CODE_ALLOW <= ROM_LIMIT
        m = dict(part=i + 1, first_unique_frame=u0, unique_frames=u1 - u0, start_min=round(f0 / 5 / 60, 2),
                 end_min=round(f1 / 5 / 60, 2), audio_chunks=c1 - c0, av_offset_vblanks=round(c0 * VB_PER_CHUNK - t_of(u0), 3),
                 asset_bytes=total)
        manifest.append(m)
        print("part%02d: %6.1f-%6.1f min  %5d frames  %5d audio chunks  assets %.2f MiB (ROM ~%.2f MiB)  A/V offset %+.2f vblank" %
              (i + 1, m["start_min"], m["end_min"], u1 - u0, c1 - c0, total / 2**20, (total + 16384) / 2**20, m["av_offset_vblanks"]))

        # ---- verification against the input: what main.c would play in this part == the original film ----
        rl = struct.unpack("<%dI" % len(li), open(os.path.join(d, "frames_idx.bin"), "rb").read())
        rd = b"".join(open(os.path.join(d, x), "rb").read() for x in ("frames1a.bin", "frames1b.bin", "frames2.bin"))
        assert rd[rl[3]:rl[4]] == data[idx[u0 + 3]:idx[u0 + 4]] and len(rd) == rl[-1]
        for j in range(0, u1 - u0, 1):
            assert rd[rl[j]:rl[j + 1]] == data[idx[u0 + j]:idx[u0 + j + 1]], "frame %d differs" % (u0 + j)
        rp = [struct.unpack("<I16H", open(os.path.join(d, "palette.bin"), "rb").read()[k:k + 36]) for k in range(0, 36 * len(ls), 36)]
        seg_at = lambda table, f: max((s for s in table if s[0] <= f), key=lambda s: s[0])[1:]   # main.c's palette pick
        for f in range(0, f1 - f0, 1):
            assert seg_at(rp, f) == seg_at(segs, f0 + f), "palette differs at frame %d" % (f0 + f)
        ra = b"".join(open(os.path.join(d, x), "rb").read() for x in ("audio_a.bin", "audio_b.bin"))
        rs = struct.unpack("<%dI" % (2 * (c1 - c0)), open(os.path.join(d, "audio_state.bin"), "rb").read())
        assert ra == audio[c0 * CHUNK:c1 * CHUNK] and rs == states[c0 * 2:c1 * 2]
        assert abs(c0 * VB_PER_CHUNK - t_of(u0)) <= 2, "audio/video offset too large"
    assert cuts[0] == 0 and cuts[-1] == nu and all(cuts[k] < cuts[k + 1] for k in range(n))
    json.dump(manifest, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    print("verified: every frame, palette switch, audio chunk and seek state matches the original")


if __name__ == "__main__":
    main()
