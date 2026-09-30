/* Host test: decodes an encoded part with the REAL adpcm.h, both sequentially and by seeking
   to random chunks via audio_state.bin, and checks they agree (bit-exact) and measures SNR vs source.
   usage: test_adpcm audio.bin audio_state.bin source_L.raw source_R.raw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../adpcm.h"
static void *slurp(const char*p,long*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}fseek(f,0,SEEK_END);*n=ftell(f);rewind(f);void*b=malloc(*n+1);fread(b,1,*n,f);fclose(f);return b;}
int main(int c,char**v){
    long na,ns,nl,nr; u8*a=slurp(v[1],&na); u32*st=slurp(v[2],&ns); short*L=slurp(v[3],&nl),*R=slurp(v[4],&nr);
    long chunks=na/304; if(ns/8!=chunks){printf("FAIL: state count %ld != chunks %ld\n",ns/8,chunks);return 1;}
    s8 s1[2][304],s2[2][304]; int pred[2]={0,0},sidx[2]={0,0}; long bad=0; double sig=0,err=0;
    srand(1); long seeks=0;
    for(long k=0;k<chunks;k++){
        for(int ch=0;ch<2;ch++) adpcm_decode(a+k*304+ch*152,s1[ch],152,&pred[ch],&sidx[ch]);
        /* the state the player would load for this chunk must equal the running state at its start */
        if(k%97==0||k==chunks-1){ /* seek check on a sample of chunks */
            int p2[2],i2[2];
            for(int ch=0;ch<2;ch++){u32 sv=st[k*2+ch];p2[ch]=(short)(sv&0xFFFF);i2[ch]=(sv>>16)&0xFF;
                adpcm_decode(a+k*304+ch*152,s2[ch],152,&p2[ch],&i2[ch]);}
            if(memcmp(s1,s2,sizeof s1)||p2[0]!=pred[0]||p2[1]!=pred[1]) bad++;
            seeks++;
        }
        for(int i=0;i<304;i++){ long j=k*304+i; if(j<nl/2){
            double l=L[j]/256.0-s1[0][i], r=R[j]/256.0-s1[1][i];
            sig+=(L[j]/256.0)*(L[j]/256.0)+(R[j]/256.0)*(R[j]/256.0); err+=l*l+r*r; } }
    }
    printf("chunks=%ld seek-checks=%ld mismatches=%ld SNR(8-bit output)=%.1f dB\n",chunks,seeks,bad,10*log10(sig/(err+1e-9)));
    return bad?1:0;
}
