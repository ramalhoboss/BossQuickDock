/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "opus_wire.h"
#include <string.h>
static void put(unsigned char *p,uint64_t v,int n){for(int i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i));}
int opus_wire_ipv4(const char *s,unsigned char out[4]){if(!s||!*s)return 0;for(int i=0;i<4;i++){unsigned v=0,n=0;while(*s>='0'&&*s<='9'){v=v*10+(unsigned)(*s++-'0');if(++n>3||v>255)return 0;}if(!n)return 0;out[i]=(unsigned char)v;if(i<3){if(*s++!='.')return 0;}else if(*s)return 0;}return out[0]>0&&out[0]<224&&out[3]!=255;}
int opus_wire_packet(unsigned char *out,size_t cap,uint64_t session,uint32_t seq,uint64_t pts,uint64_t elapsed,const void *payload,size_t bytes){
 if(!out||(!payload&&bytes)||bytes>1200||cap<48+bytes)return -1;
 memset(out,0,48);memcpy(out,"BQDOPUS1",8);put(out+8,1,2);put(out+10,48,2);put(out+12,session,8);put(out+20,seq,4);put(out+24,pts,8);put(out+32,elapsed,8);put(out+40,bytes,2);put(out+42,bytes?480:0,2);put(out+44,48000,4);if(bytes)memcpy(out+48,payload,bytes);return (int)(48+bytes);
}
