#!/usr/bin/env python3
"""
clipenc.py - squash a video into the "Konami clip" (ROM 1 only), deliberately compressed to hell.

    python3 tools/clipenc.py input.mp4 --out clip [--target 380]

Format (what main.c expects for the Konami stream):
  clip_frames.bin   RLE frames, 120x34 stored, row-repeat pairs (0,0) make it 120x68; pixel values 1..16
  clip_idx.bin      u32 byte offset per unique frame (+ final total); 2.5 unique frames/s like the film
  clip_palette.bin  per scene { u32 first_frame; u16 col[16] } (36 bytes), first_frame counts 5 fps steps
  clip_audio.bin    MONO 2-bit ADPCM2, 4539.25 Hz, 76 bytes per 304-sample chunk (= 4 vblanks)
  clip_state.bin    u32 per chunk: predictor | step index << 16 (for seeking)
Needs: python3, numpy, Pillow, ffmpeg, gcc.
"""
import argparse, os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageFilter, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
W, R = 120, 34
UFPS = 2.5                      # unique frames per second (the player shows each for 2 of its 5-fps steps)
AUDIO_HZ = 4539.25
CH = 304
NCOL = 16

def build(name):
    exe = os.path.join(tempfile.gettempdir(), name)
    src = os.path.join(HERE, name + '.c')
    subprocess.run(['gcc', '-O2', '-o', exe, src, '-lm'], check=True, stderr=subprocess.DEVNULL)
    return exe

def read_frames(path):
    cmd = ['ffmpeg', '-v', 'error', '-i', path, '-an', '-vf', f'fps={UFPS},scale={W}:{R}:flags=area',
           '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n = W * R * 3; out = []
    while True:
        b = p.stdout.read(n)
        if len(b) < n: break
        out.append(np.frombuffer(b, np.uint8).reshape(R, W, 3).copy())
    p.wait(); return out

def bgr555(r, g, b): return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input'); ap.add_argument('--out', default='clip')
    ap.add_argument('--target', type=float, default=380, help='video bytes per unique frame (average)')
    ap.add_argument('--cut', type=float, default=22.0); ap.add_argument('--maxseg', type=int, default=20)
    ap.add_argument('--preview', action='store_true', help='also keep preview_audio.raw (exact decoder output)')
    ap.add_argument('--seconds', type=float, help='only encode the first N seconds (tests)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rle_exe = build('clip_rle'); a2_exe = build('adpcm2_enc')

    # ---------------- video ----------------
    frames = read_frames(a.input)
    if a.seconds: frames = frames[:int(a.seconds * UFPS)]
    nu = len(frames); print('unique frames', nu)
    # scene segmentation -> one 16-colour palette per scene
    segs = [0]; last = 0; prev = frames[0].astype(np.int16).mean(2)
    for i in range(1, nu):
        cur = frames[i].astype(np.int16).mean(2)
        if (np.abs(cur - prev).mean() > a.cut and i - last >= 3) or i - last >= a.maxseg: segs.append(i); last = i
        prev = cur
    segs.append(nu); print('scenes', len(segs) - 1)
    rle_out = []; sizes = []; pal_out = []; carry = 0.0
    for si in range(len(segs) - 1):
        lo, hi = segs[si], segs[si + 1]
        seg = [ImageEnhance.Contrast(Image.fromarray(f)).enhance(1.15) for f in frames[lo:hi]]
        step = max(1, len(seg) // 12); smp = seg[::step][:12]
        sheet = Image.new('RGB', (W * 4, R * 3))
        for i, f in enumerate(smp): sheet.paste(f, ((i % 4) * W, (i // 4) * R))
        pi = sheet.quantize(colors=NCOL, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pp = (pi.getpalette() + [0] * 768)[:NCOL * 3]
        q = b''.join(np.asarray(f.quantize(palette=pi, dither=Image.Dither.NONE), np.uint8).tobytes() for f in seg)
        with tempfile.TemporaryDirectory() as td:
            open(td + '/pal.rgb', 'wb').write(bytes(pp))
            r = subprocess.run([rle_exe, td + '/pal.rgb', str(a.target), td + '/o.bin', td + '/s.txt', str(carry)],
                               input=q, capture_output=True, check=True)
            carry = float(r.stderr.decode().strip().split()[-1])
            rle_out.append(open(td + '/o.bin', 'rb').read())
            sizes.append(np.loadtxt(td + '/s.txt', ndmin=2)[:, 0].astype(int))
        pal_out.append((lo * 2, [bgr555(*pp[i * 3:i * 3 + 3]) for i in range(NCOL)]))
    data = b''.join(rle_out); sz = np.concatenate(sizes)
    idx = np.concatenate([[0], np.cumsum(sz)]).astype('<u4')
    open(f'{a.out}/clip_frames.bin', 'wb').write(data)
    open(f'{a.out}/clip_idx.bin', 'wb').write(idx.tobytes())
    open(f'{a.out}/clip_palette.bin', 'wb').write(b''.join(struct.pack('<I16H', f, *c) for f, c in pal_out))
    print('video: %d bytes, %.0f B/frame avg, %d palette scenes' % (len(data), len(data) / nu, len(pal_out)))

    # ---------------- audio (mono, 2-bit ADPCM2) ----------------
    dur = nu / UFPS
    tmp = tempfile.mkdtemp()
    cmd = ['ffmpeg', '-v', 'error', '-y', '-i', a.input, '-vn', '-t', str(dur), '-ac', '1',
           '-af', 'highpass=f=60,lowpass=f=1900,aresample=4539', '-f', 's16le', '-acodec', 'pcm_s16le', tmp + '/m.raw']
    subprocess.run(cmd, check=True)
    x = np.fromfile(tmp + '/m.raw', '<i2').astype(np.float64)
    pk = np.percentile(np.abs(x), 99.8) + 1
    y = np.tanh(x * min(6.0, 28000 / pk) / 22000) * 22000 * 1.15          # soft-clip: 2 bits like it LOUD
    nch = int(np.ceil(nu * 2 * 65536 / 5486 / 4)) + 1                       # chunks needed to cover the picture (4 vblanks each)
    pad = np.zeros(nch * CH); pad[:min(len(y), len(pad))] = y[:len(pad)]
    np.clip(pad, -32768, 32767).astype('<i2').tofile(tmp + '/n.raw')
    r = subprocess.run([a2_exe, f'{a.out}/clip_audio.bin', f'{a.out}/clip_state.bin', tmp + '/recon.raw'],
                       stdin=open(tmp + '/n.raw', 'rb'), capture_output=True, check=True)
    print('audio:', r.stderr.decode().strip())
    if a.preview: os.replace(tmp + '/recon.raw', f'{a.out}/preview_audio.raw')
    tot = sum(os.path.getsize(f'{a.out}/{n}') for n in ('clip_frames.bin', 'clip_idx.bin', 'clip_palette.bin', 'clip_audio.bin', 'clip_state.bin'))
    print('TOTAL clip assets: %d bytes (%.3f MiB)' % (tot, tot / 2**20))

if __name__ == '__main__':
    main()
