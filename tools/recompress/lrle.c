/* lossy RLE: argv: pal.rgb(N*3 bytes, N<=256) target_bytes_per_frame out.bin sizes.txt ; stdin: frames of 8160 idx bytes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define FB 8160
#define W 120
static int D[256][256];
static int enc(const uint8_t*px,int T,uint8_t*out){
  int n=0;
  for(int y=0;y<68;y++){const uint8_t*r=px+y*W;int c=r[0],run=1;
    for(int x=1;x<W;x++){int p=r[x];
      if(run>0&&run<255&&(p==c||D[p][c]<=T)){run++;}
      else{ if(run>0){out[n++]=run;out[n++]=c;} c=p;run=1; }
    }
    out[n++]=run;out[n++]=c;
  }
  return n;}
int main(int argc,char**argv){
  FILE*fp=fopen(argv[1],"rb");uint8_t pal[768]={0};int np=fread(pal,3,256,fp);fclose(fp);
  double target=atof(argv[2]);FILE*fo=fopen(argv[3],"wb"),*fs=fopen(argv[4],"w");
  for(int a=0;a<256;a++)for(int b=0;b<256;b++){int dr=pal[a*3]-pal[b*3],dg=pal[a*3+1]-pal[b*3+1],db=pal[a*3+2]-pal[b*3+2];D[a][b]=2*dr*dr+4*dg*dg+3*db*db;}
  static uint8_t px[FB],out[FB*2+64],best[FB*2+64];double carry=argc>5?atof(argv[5]):0;
  static const int Ts[]={0,30,60,100,160,240,340,480,680,950,1300,1800,2500,3500,5000,8000,14000,30000,1000000};
  int nT=sizeof(Ts)/sizeof(int);
  while(fread(px,1,FB,stdin)==FB){
    double cap=target+(carry>0?(carry<target*2?carry:target*2):carry*0.5); if(cap<target*0.5)cap=target*0.5;
    int n=0,k;
    for(k=0;k<nT;k++){n=enc(px,Ts[k],out); if(n<=cap)break;}
    if(k==nT){k=nT-1;n=enc(px,Ts[k],out);}
    fwrite(out,1,n,fo);fprintf(fs,"%d %d\n",n,Ts[k]);carry+=target-n;
  }
  fprintf(stderr,"%f\n",carry);return 0;}
