import numpy as np, json, scipy.signal as ss, subprocess
man=json.load(open('../proj/film/parts/manifest.json'))
HZ=16777216/1848
segs=[]
for i,m in enumerate(man):
    a=np.fromfile(f'pcm{m["part"]:02d}.raw','<i2').reshape(-1,2)
    if i+1<len(man):
        L=round(man[i+1]['first_frame']/5*HZ)-round(m['first_frame']/5*HZ)
        a=a[:L]
    segs.append(a)
pcm=np.concatenate(segs).astype(np.float32)
print(pcm.shape, pcm.shape[0]/HZ/60,'min')
# lowpass ~2000Hz (fs 9078.5) then decimate by 2
h=ss.firwin(63,1900,fs=HZ,window='hamming')
out=np.stack([ss.oaconvolve(pcm[:,c],h,mode='same')[::2] for c in (0,1)],1)
# gentle normalisation: boost quiet audio (4-bit ADPCM at low rate benefits from full-scale)
pk=np.percentile(np.abs(out),99.9); g=min(3.0,26000/pk); print('peak',pk,'gain',g)
out=np.clip(out*g,-32768,32767).astype('<i2')
n=(len(out)//304+1)*304
pad=np.zeros((n,2),'<i2'); pad[:len(out)]=out
open('L.raw','wb').write(pad[:,0].tobytes()); open('R.raw','wb').write(pad[:,1].tobytes())
print('chunks',n//304)
for c,nm in ((0,'L'),(1,'R')):
    subprocess.run(['./adpcm_enc',f'{nm}.adpcm',f'{nm}.state'],stdin=open(f'{nm}.raw','rb'),check=True)
L=np.fromfile('L.adpcm',np.uint8).reshape(-1,152);R=np.fromfile('R.adpcm',np.uint8).reshape(-1,152)
open('audio.bin','wb').write(np.concatenate([L,R],1).tobytes())
st=np.stack([np.fromfile('L.state','<u4'),np.fromfile('R.state','<u4')],1)
open('audio_state.bin','wb').write(st.astype('<u4').tobytes())
import os;print(os.path.getsize('audio.bin')/2**20,'MiB',os.path.getsize('audio_state.bin')/2**20)
