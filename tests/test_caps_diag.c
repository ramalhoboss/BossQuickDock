#undef NDEBUG
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "caps_diag.h"
#include "stream_diag.h"
int main(void){
 hb_caps_diag d; hb_diag_time t={0};
 const unsigned char sbc[]={1,0,7,6,0,0,0x11,0x15,2,53,8,0};
 const unsigned char aac[]={7,8,0,2,0x80,1,0x8c,0,0,0};
 const unsigned char vendor[]={7,9,0,255,0x4f,0,0,0,1,0,0x12};
 const unsigned char truncated[]={7,8,0,2,0x80};
 const unsigned char shortaac[]={7,7,0,2,0x80,1,0x8c,0,0};
 const unsigned char nonaudio[]={7,2,0x10,0x77};
 const unsigned char odd[]={1};
 assert(hb_caps_diag_parse(sbc,sizeof sbc,&d)&&d.count==1&&d.codecs[0].codec==0&&d.delay_report);
 assert(hb_caps_diag_parse(aac,sizeof aac,&d)&&d.count==1&&d.codecs[0].codec==2&&d.codecs[0].ie_len==6);
 assert(hb_caps_diag_parse(vendor,sizeof vendor,&d)&&d.codecs[0].codec==255);
 assert(!hb_caps_diag_parse(truncated,sizeof truncated,&d)&&!d.valid);
 assert(!hb_caps_diag_parse(shortaac,sizeof shortaac,&d)&&!d.valid);
 assert(!hb_caps_diag_parse(odd,sizeof odd,&d));
 assert(hb_caps_diag_parse(nonaudio,sizeof nonaudio,&d)&&d.codecs[0].media==1&&d.codecs[0].codec==0x77);
 assert(!hb_caps_diag_parse(0,0,&d)&&!hb_caps_diag_parse(sbc,-1,&d)&&!hb_caps_diag_parse(sbc,sizeof sbc,0));
 unsigned char many[260];for(int i=0;i<65;i++){many[i*4]=7;many[i*4+1]=2;many[i*4+2]=0x10;many[i*4+3]=0x77;}
 assert(!hb_caps_diag_parse(many,sizeof many,&d)&&!d.valid);
 hb_diag_time_add(&t,100,110);hb_diag_time_add(&t,200,240);hb_diag_time_add(&t,300,300);
 assert(t.calls==3&&t.total_us==50&&t.max_us==40&&hb_diag_time_avg(&t)==16);
 hb_diag_time_add(&t,500,400);hb_diag_time_add(&t,0,1);
 assert(t.invalid==2&&t.calls==3&&t.total_us==50);
 memset(&t,0,sizeof t);assert(hb_diag_time_avg(&t)==0);
 puts("PASS: capability parsing and timing aggregates (malformed, short AAC, unknown, overflow, clock errors)");return 0;
}
