#!/usr/bin/env python3
"""
gbatool.py - encode a long video into a set of GBA ROM "parts" (120x68 @ 5fps, 8bpp RLE, stereo ADPCM).

A GBA cartridge can address at most 32 MiB, so a feature-length film cannot fit in one ROM. `film` cuts the
video greedily into parts that each fill (just under) 32 MiB, giving every part its own 256-colour palette.

  python3 tools/gbatool.py film input.webm [--out parts] [--start SEC] [--end SEC] [--max-parts N]

Formats (must match main.c):
  frames_idx.bin : u32 byte offset per frame into the RLE stream (+1 final entry = total)
  frames1a/1b/2.bin : RLE stream (run,value byte pairs, run 1-255), cut at 24 MiB, first 24 MiB halved (GitHub file limit)
  palette.bin    : 256 x u16 BGR555
  audio_a/b.bin  : per 304-sample chunk (2 vblanks): 152 bytes left ADPCM then 152 bytes right ADPCM, cut in two at a chunk boundary
  audio_state.bin: per chunk, u32 [left, right] = predictor(s16) | stepindex<<16 at chunk start (for seeking)
"""
import argparse, json, math, os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
VID_W, VID_H = 120, 68
FRAME_BYTES = VID_W * VID_H
FPS_NUM, FPS_DEN = 5486, 65536
AUDIO_HZ = 16777216 / 1848                 # 9078.5 Hz
SPC = 304                                  # samples per chunk per channel
CHUNK_BYTES = SPC // 2                     # per channel
ROM_LIMIT = 32 * 1024 * 1024               # GBA cartridge address space
CODE_MARGIN = 96 * 1024                    # player code + header + alignment
SPLIT = 24 * 1024 * 1024

def chunks_for_frames(n):
    return max(1, math.ceil(math.ceil(n * FPS_DEN / FPS_NUM) / 2))

def part_size(rle_bytes, n):
    ch = chunks_for_frames(n)
    return rle_bytes + 4 * (n + 1) + 512 + ch * (2 * CHUNK_BYTES) + ch * 8 + CODE_MARGIN

def bgr555(rgb):
    w = [(r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) for r, g, b in rgb]
    w += [0] * (256 - len(w))
    return struct.pack('<256H', *w[:256])

def make_palette(frames):
    step = max(1, len(frames) // 96)
    sample = frames[::step][:96]
    cols = 12; rows = math.ceil(len(sample) / cols)
    sheet = Image.new('RGB', (cols * VID_W, rows * VID_H))
    for i, f in enumerate(sample):
        sheet.paste(Image.fromarray(f), ((i % cols) * VID_W, (i // cols) * VID_H))
    pal = sheet.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    flat = (pal.getpalette() or []) + [0] * 768
    return pal, bgr555([tuple(flat[i*3:i*3+3]) for i in range(256)])

def rle(px):
    """px: uint8 array (FRAME_BYTES,) -> RLE bytes, runs capped at 255."""
    b = np.flatnonzero(np.diff(px)) + 1
    starts = np.concatenate(([0], b)); lens = np.diff(np.concatenate((starts, [len(px)])))
    vals = px[starts]
    if lens.max() > 255:
        L, V = [], []
        for l, v in zip(lens.tolist(), vals.tolist()):
            while l > 255: L.append(255); V.append(v); l -= 255
            L.append(l); V.append(v)
        lens = np.array(L, dtype=np.uint8); vals = np.array(V, dtype=np.uint8)
    out = np.empty(len(lens) * 2, dtype=np.uint8)
    out[0::2] = lens; out[1::2] = vals
    return out.tobytes()

def unrle(data):
    a = np.frombuffer(data, dtype=np.uint8)
    return np.repeat(a[1::2], a[0::2])

def video_reader(path, start, end):
    cmd = ['ffmpeg', '-v', 'error']
    if start: cmd += ['-ss', str(start)]
    cmd += ['-i', path]
    if end is not None: cmd += ['-t', str(end - (start or 0))]
    cmd += ['-an', '-vf', f'fps=5,scale={VID_W}:{VID_H}:flags=lanczos', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = VID_W * VID_H * 3
    while True:
        b = p.stdout.read(n)
        if len(b) < n: break
        yield np.frombuffer(b, dtype=np.uint8).reshape(VID_H, VID_W, 3)
    p.wait()

def adpcm(pcm_mono, tmp):
    """int16 mono (multiple of 304) -> (adpcm bytes, state bytes) via the C encoder."""
    exe = os.path.join(HERE, 'adpcm_enc')
    if not os.path.exists(exe):
        subprocess.run(['gcc', '-O2', '-o', exe, exe + '.c'], check=True)
    ob, sb = os.path.join(tmp, 'a.bin'), os.path.join(tmp, 's.bin')
    subprocess.run([exe, ob, sb], input=pcm_mono.astype('<i2').tobytes(), check=True)
    return open(ob, 'rb').read(), open(sb, 'rb').read()

def audio_for_part(pcm, first_frame, nframes, tmp):
    """pcm: memmap of stereo int16 @ 2*AUDIO_HZ (18157 Hz, already low-passed). Decimate by 2 -> 9078.5 Hz."""
    want = chunks_for_frames(nframes) * SPC
    a0 = round(first_frame / 5 * AUDIO_HZ)
    seg = np.zeros((want * 2, 2), dtype=np.int32)
    src = pcm[a0 * 2:(a0 + want) * 2]
    seg[:len(src)] = src
    dec = ((seg[0::2] + seg[1::2]) // 2).astype(np.int16)
    (lb, ls), (rb, rs) = adpcm(dec[:, 0], tmp), adpcm(dec[:, 1], tmp)
    nch = len(lb) // CHUNK_BYTES
    audio = np.concatenate([np.frombuffer(lb, np.uint8).reshape(nch, CHUNK_BYTES),
                            np.frombuffer(rb, np.uint8).reshape(nch, CHUNK_BYTES)], axis=1).tobytes()
    st = np.stack([np.frombuffer(ls, '<u4'), np.frombuffer(rs, '<u4')], axis=1).reshape(-1).astype('<u4').tobytes()
    return audio, st, dec

def wr(d, name, data):
    with open(os.path.join(d, name), 'wb') as f: f.write(data)

def cmd_film(a):
    src = a.input
    pcm_path = a.pcm or os.path.join(a.work, 'pcm18157.raw')
    if not os.path.exists(pcm_path):
        os.makedirs(a.work, exist_ok=True)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-vn', '-ac', '2', '-af', 'lowpass=f=4000,aresample=18157',
                        '-f', 's16le', pcm_path], check=True)
    pcm = np.memmap(pcm_path, dtype='<i2', mode='r').reshape(-1, 2)
    off = round((a.start or 0) * 5)          # frame index of the first frame (for audio alignment)
    reader = video_reader(src, a.start, a.end)
    budget = ROM_LIMIT
    os.makedirs(a.out, exist_ok=True)
    manifest = []; pno = 0; first = off
    tmp = tempfile.mkdtemp()
    buf = []
    while (a.max_parts is None or pno < a.max_parts):
        while len(buf) < 5000:
            f = next(reader, None)
            if f is None: break
            buf.append(f)
        if not buf: break
        pno += 1
        pal_img, pal_bytes = make_palette(buf[:3800])
        rle_parts, offs, total, n = [], [0], 0, 0
        for fr in buf:
            q = np.asarray(Image.fromarray(fr).quantize(palette=pal_img, dither=Image.Dither.NONE), dtype=np.uint8).reshape(-1)
            r = rle(q)
            if part_size(total + len(r), n + 1) > budget and n > 0: break
            rle_parts.append(r); total += len(r); n += 1; offs.append(total)
        buf = buf[n:]
        d = os.path.join(a.out, f'part{pno:02d}'); os.makedirs(d, exist_ok=True)
        data = b''.join(rle_parts)
        audio, states, _ = audio_for_part(pcm, first, n, tmp)
        f1 = data[:SPLIT]; h = len(f1) // 2
        wr(d, 'frames1a.bin', f1[:h]); wr(d, 'frames1b.bin', f1[h:]); wr(d, 'frames2.bin', data[SPLIT:])
        wr(d, 'frames_idx.bin', struct.pack(f'<{len(offs)}I', *offs))
        wr(d, 'palette.bin', pal_bytes); wr(d, 'audio_state.bin', states)
        ac = (len(audio) // (2 * CHUNK_BYTES)) // 2 * (2 * CHUNK_BYTES)      # halve on a chunk boundary
        wr(d, 'audio_a.bin', audio[:ac]); wr(d, 'audio_b.bin', audio[ac:])
        size = sum(os.path.getsize(os.path.join(d, x)) for x in os.listdir(d))
        # round-trip check of a few frames
        for k in (0, n // 2, n - 1):
            assert len(unrle(data[offs[k]:offs[k + 1]])) == FRAME_BYTES, 'RLE round-trip failed'
        m = dict(part=pno, first_frame=first, frames=n, start_s=first / 5, end_s=(first + n) / 5,
                 assets_bytes=size, rom_estimate=size + CODE_MARGIN)
        manifest.append(m)
        print(f"part{pno:02d}: {m['start_s']/60:6.1f}-{m['end_s']/60:6.1f} min  {n} frames  ROM~{(size+CODE_MARGIN)/2**20:.2f} MiB", flush=True)
        first += n
    json.dump(manifest, open(os.path.join(a.out, 'manifest.json'), 'w'), indent=1)
    print(f'{len(manifest)} parts, {sum(m["frames"] for m in manifest)/5/60:.1f} min total')

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('film', help='encode a video into ROM parts')
    f.add_argument('input'); f.add_argument('--out', default='parts'); f.add_argument('--work', default='work')
    f.add_argument('--pcm', help='pre-extracted 18157 Hz stereo s16le (skips the audio ffmpeg pass)')
    f.add_argument('--start', type=float); f.add_argument('--end', type=float); f.add_argument('--max-parts', type=int)
    f.set_defaults(func=cmd_film)
    a = p.parse_args(); a.func(a)

if __name__ == '__main__':
    main()
