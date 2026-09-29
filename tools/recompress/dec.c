#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static const short step_tab[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7847,8631,9494,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const signed char idx_tab[8]={-1,-1,-1,-1,2,4,6,8};
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"),*o=fopen(argv[2],"wb");
  uint8_t c[304]; int pred[2]={0,0},sidx[2]={0,0};
  while(fread(c,1,304,f)==304){
    int16_t out[2][304];
    for(int ch=0;ch<2;ch++)for(int i=0;i<152;i++){uint8_t b=c[ch*152+i];
      for(int k=0;k<2;k++){int nib=k?(b>>4):(b&15);int step=step_tab[sidx[ch]];int diff=step>>3;
        if(nib&1)diff+=step>>2;if(nib&2)diff+=step>>1;if(nib&4)diff+=step;
        if(nib&8)pred[ch]-=diff;else pred[ch]+=diff;pred[ch]-=pred[ch]>>9;
        if(pred[ch]>32767)pred[ch]=32767;if(pred[ch]<-32768)pred[ch]=-32768;
        sidx[ch]+=idx_tab[nib&7];if(sidx[ch]<0)sidx[ch]=0;if(sidx[ch]>88)sidx[ch]=88;
        out[ch][i*2+k]=pred[ch];}}
    for(int i=0;i<304;i++){fwrite(&out[0][i],2,1,o);fwrite(&out[1][i],2,1,o);}
  }
  return 0;}
