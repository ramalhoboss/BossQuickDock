/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_mux.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p32(unsigned char *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static void p64(unsigned char *p,uint64_t v){for(int i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i));}
static int page(FILE *f,const unsigned char *packet,int n,int flags,uint64_t granule,uint32_t seq){
 unsigned char b[8500];int seg=n/255+1,header=27+seg;uint32_t crc=0;
 if(n<0||n>8192||header+n>(int)sizeof b)return -1;
 memset(b,0,(size_t)header);memcpy(b,"OggS",4);b[5]=(unsigned char)flags;p64(b+6,granule);p32(b+14,0x42514437u);p32(b+18,seq);b[26]=(unsigned char)seg;
 for(int i=0;i<seg;i++)b[27+i]=(unsigned char)(i<seg-1?255:n%255);
 memcpy(b+header,packet,(size_t)n);
 for(int i=0;i<header+n;i++){crc^=(uint32_t)b[i]<<24;for(int k=0;k<8;k++)crc=(crc&0x80000000u)?(crc<<1)^0x04c11db7u:crc<<1;}
 p32(b+22,crc);return fwrite(b,1,(size_t)(header+n),f)==(size_t)(header+n)?0:-1;
}
int native_mux(const char *input,const char *output,int format){
 FILE *in=NULL,*out=NULL;unsigned char h[32],r[32],p[8192],a[7];int rc=-1;uint32_t seq=2;uint64_t samples=0;unsigned count=0;struct stat st;long long remaining;
 if(format!=1&&format!=2)return -1;
 if(stat(input,&st)||st.st_size<32||st.st_size>8*1024*1024)return -1;remaining=(long long)st.st_size-32;
 in=fopen(input,"rb");if(!in)return -1;
 if(fread(h,1,32,in)!=32||memcmp(h,"BQDNCP01",8)||le32(h+8)!=1||le32(h+12)!=(uint32_t)format||le32(h+16)!=48000||le32(h+20)!=2||le32(h+24)!=(uint32_t)(format==1?1024:480)||le32(h+28)!=240000)goto done;
 out=fopen(output,"wb");if(!out)goto done;
 if(format==2){unsigned char head[19]={'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};unsigned char tags[29]={'O','p','u','s','T','a','g','s',13,0,0,0,'B','o','s','s','Q','u','i','c','k','D','o','c','k',0,0,0,0};if(page(out,head,19,2,0,0)||page(out,tags,29,0,0,1))goto done;}
 while(remaining){if(remaining<32||fread(r,1,32,in)!=32)goto done;remaining-=32;uint32_t n=le32(r+24);if(n==0||n>8192||n>remaining||fread(p,1,n,in)!=n)goto done;remaining-=n;
  if(format==1){unsigned len=n+7;if(len>8191)goto done;a[0]=0xff;a[1]=0xf1;a[2]=0x4c;a[3]=(unsigned char)(0x80|(len>>11));a[4]=(unsigned char)(len>>3);a[5]=(unsigned char)(((len&7)<<5)|0x1f);a[6]=0xfc;if(fwrite(a,1,7,out)!=7||fwrite(p,1,n,out)!=n)goto done;}
  else { /* Require the validated 10-ms CELT stereo packet form. */
   if((p[0]>>3)!=30||(p[0]&3)!=0||!(p[0]&4))goto done;
   samples+=480;if(page(out,p,(int)n,remaining==0?4:0,samples,seq++))goto done;
  }
  if(++count>100000)goto done;
 }
 if(count&&fflush(out)==0)rc=0;
done:
 if(out&&fclose(out)!=0)rc=-1;if(in)fclose(in);if(rc)remove(output);return rc;
}
