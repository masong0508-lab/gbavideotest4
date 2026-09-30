import numpy as np, json, subprocess, struct, sys, os
from PIL import Image, ImageFilter
P='../proj/film/parts/'; man=json.load(open(P+'manifest.json'))
NCOL=16; BLUR=0.5; MAXSEG=60; MINSEG=6; CUT=22.0
TARGET=float(sys.argv[1]) if len(sys.argv)>1 else 2550
def unrle(b):
    a=np.frombuffer(b,np.uint8); return np.repeat(a[1::2],a[0::2])
# --- load unique (even global) frames as RGB ---
frames=[]
for m in man:
    d=P+f'part{m["part"]:02d}/'
    idx=np.frombuffer(open(d+'frames_idx.bin','rb').read(),'<u4')
    data=b''.join(open(d+n,'rb').read() for n in ('frames1a.bin','frames1b.bin','frames2.bin')) if os.path.exists(d+'frames1a.bin') else open(d+'frames1.bin','rb').read()+open(d+'frames2.bin','rb').read()
    pal=np.frombuffer(open(d+'palette.bin','rb').read(),'<u2').astype(np.int32)
    rgb=np.stack([((pal&31)<<3)|((pal&31)>>2),(((pal>>5)&31)<<3)|(((pal>>5)&31)>>2),(((pal>>10)&31)<<3)|(((pal>>10)&31)>>2)],1).astype(np.uint8)
    for k in range(m['frames']):
        g=m['first_frame']+k
        if g%2==0:
            frames.append(rgb[unrle(data[idx[k]:idx[k+1]])].reshape(68,120,3))
NF=sum(m['frames'] for m in man); print('unique',len(frames),'total',NF,flush=True)
# --- scene segmentation ---
gray=np.array([f.astype(np.int16).sum(2)//3 for f in frames[::1]],dtype=np.int16) if False else None
segs=[0]; last=0
prev=frames[0][::2,::2].astype(np.int16).mean(2)
for i in range(1,len(frames)):
    cur=frames[i][::2,::2].astype(np.int16).mean(2)
    diff=np.abs(cur-prev).mean(); prev=cur
    if (diff>CUT and i-last>=MINSEG) or i-last>=MAXSEG:
        segs.append(i); last=i
segs.append(len(frames)); print('segments',len(segs)-1,flush=True)
# --- palette + quantize + lossy rle ---
out_rle=[]; sizes=[]; pals=[]; carry=0.0
for si in range(len(segs)-1):
    a,b=segs[si],segs[si+1]
    seg=[Image.fromarray(f).filter(ImageFilter.GaussianBlur(BLUR)) for f in frames[a:b]]
    step=max(1,len(seg)//24); smp=seg[::step][:24]
    sheet=Image.new('RGB',(120*6,68*4))
    for i,f in enumerate(smp): sheet.paste(f,((i%6)*120,(i//6)*68))
    pi=sheet.quantize(colors=NCOL,method=Image.Quantize.MEDIANCUT,dither=Image.Dither.NONE)
    pp=(pi.getpalette()+[0]*768)[:768]
    open('pal.rgb','wb').write(bytes(pp))
    q=b''.join(np.asarray(f.quantize(palette=pi,dither=Image.Dither.NONE),np.uint8).tobytes() for f in seg)
    r=subprocess.run(['./lrle','pal.rgb',str(TARGET),'o.bin','s.txt',str(carry)],input=q,capture_output=True,check=True)
    carry=float(r.stderr.decode().strip().split()[-1])
    out_rle.append(open('o.bin','rb').read()); sizes.append(np.loadtxt('s.txt',ndmin=2)[:,0].astype(int))
    w=[]
    for i in range(NCOL):
        R,G,B=pp[i*3:i*3+3]; w.append((R>>3)|((G>>3)<<5)|((B>>3)<<10))
    pals.append(w)
    if si%50==0: print(si,a,flush=True)
data=b''.join(out_rle); sz=np.concatenate(sizes)
uoffs=np.concatenate([[0],np.cumsum(sz)]).astype(np.uint32)   # per unique frame
NF_U=len(frames)
os.makedirs('out',exist_ok=True)
SPLIT=24*1024*1024
f1=data[:SPLIT]; h=len(f1)//2
open('out/frames1a.bin','wb').write(f1[:h]); open('out/frames1b.bin','wb').write(f1[h:]); open('out/frames2.bin','wb').write(data[SPLIT:])
open('out/frames_idx.bin','wb').write(uoffs.tobytes())   # per UNIQUE frame (each shown for 2 x 5fps ticks)
seg_frames=[s*2 for s in segs[:-1]]
open('out/palette.bin','wb').write(b''.join(struct.pack('<I',sf)+struct.pack('<16H',*p) for sf,p in zip(seg_frames,pals)))
open('out/seg_count.txt','w').write(str(len(pals)))
print('video bytes',len(data),'avg/unique',len(data)/len(frames))
