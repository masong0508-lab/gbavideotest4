/* adpcm_enc: mono int16 LE PCM (stdin) -> ADPCM bytes (argv[1]) + per-chunk decoder states (argv[2]).
   Mirrors adpcm.h exactly (including the leak). Chunk = 304 samples. State = pred(s16) | sidx<<16 as u32 LE.
   Input length must be a multiple of 304 samples. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static const short step_tab[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7847,8631,9494,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const signed char idx_tab[8]={-1,-1,-1,-1,2,4,6,8};
#define CH 304
int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: adpcm_enc out.bin states.bin < mono.raw\n");return 1;}
    FILE*fo=fopen(argv[1],"wb"),*fs=fopen(argv[2],"wb");
    int16_t buf[CH]; int pred=0,sidx=0; size_t n;
    while((n=fread(buf,2,CH,stdin))==CH){
        uint32_t st=(uint32_t)(pred&0xFFFF)|((uint32_t)sidx<<16); fwrite(&st,4,1,fs);
        uint8_t out[CH/2];
        for(int i=0;i<CH;i++){
            int step=step_tab[sidx], d=buf[i]-pred, nib=0;
            if(d<0){nib=8;d=-d;}
            int t=step;
            if(d>=t){nib|=4;d-=t;} t>>=1;
            if(d>=t){nib|=2;d-=t;} t>>=1;
            if(d>=t){nib|=1;}
            int diff=step>>3;
            if(nib&1)diff+=step>>2; if(nib&2)diff+=step>>1; if(nib&4)diff+=step;
            if(nib&8)pred-=diff; else pred+=diff;
            pred-=pred>>9;
            if(pred>32767)pred=32767; if(pred<-32768)pred=-32768;
            sidx+=idx_tab[nib&7]; if(sidx<0)sidx=0; if(sidx>88)sidx=88;
            if(i&1) out[i>>1]|=(uint8_t)(nib<<4); else out[i>>1]=(uint8_t)nib;
        }
        fwrite(out,1,CH/2,fo);
    }
    if(n!=0){fprintf(stderr,"input not a multiple of %d samples\n",CH);return 2;}
    fclose(fo);fclose(fs);return 0;
}
