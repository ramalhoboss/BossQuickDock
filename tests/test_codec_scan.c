#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../src/a2dp/avdtp.c"
static long clock_ms=10000;
static int scenario,signal_id,seid_id,label_id,tx_count,fallbacks;
static char discovery[512],logs[24000];
long now_ms(void){return clock_ms;}
const char *hb_codec_key(int c){(void)c;return "sbc";}
void diag_set(const char *key,const char *fmt,...){if(!strcmp(key,"codec discovery")){va_list ap;va_start(ap,fmt);vsnprintf(discovery,sizeof discovery,fmt,ap);va_end(ap);}}
void log_line(const char *fmt,...){va_list ap;va_start(ap,fmt);size_t n=strlen(logs);vsnprintf(logs+n,sizeof logs-n,fmt,ap);va_end(ap);n=strlen(logs);if(n+2<sizeof logs){logs[n]='\n';logs[n+1]=0;}}
int btlink_l2_send(btlink *l,unsigned scid,const unsigned char *d,int n){(void)l;assert(scid==0x40&&n>=2);signal_id=d[1]&63;label_id=d[0]>>4;seid_id=n>2?d[2]>>2:0;assert(signal_id==1||signal_id==2||signal_id==12);if(signal_id==2)fallbacks++;tx_count++;return 1;}
int btlink_wait_rx(btlink *l,unsigned scid,unsigned char *out,int max,int wait){
 (void)l;(void)scid;assert(max>=32);
 if(scenario==5&&seid_id==2&&signal_id!=1){clock_ms+=wait;return 0;}
 clock_ms+=10;
 out[0]=AV_HDR(label_id,AV_PKT_SINGLE,AV_MSG_ACCEPT);out[1]=(unsigned char)signal_id;
 if(signal_id==1){const unsigned char ep[]={4,8,10,8,12,8,16,0};memcpy(out+2,ep,sizeof ep);return scenario==7?9:10;}
 if(scenario==2&&seid_id==2&&signal_id==12){out[0]=AV_HDR(label_id,AV_PKT_SINGLE,AV_MSG_REJECT);out[2]=0x19;return 3;}
 if(scenario==4&&seid_id==2){out[0]=AV_HDR(label_id,AV_PKT_START,AV_MSG_ACCEPT);out[2]=1;out[3]=7;return 4;}
 const unsigned char sbc[]={1,0,7,6,0,0,0x11,0x15,2,53,8,0};
 const unsigned char aac[]={7,8,0,2,0x80,1,0x8c,0,0,0};
 const unsigned char vendor[]={7,9,0,255,0x4f,0,0,0,1,0,0x12};
 const unsigned char *p=seid_id==1?sbc:seid_id==2?aac:vendor;int n=seid_id==1?sizeof sbc:seid_id==2?sizeof aac:sizeof vendor;
 memcpy(out+2,p,(size_t)n);
 if(scenario==3&&seid_id==2)return 6;
 if(scenario==6)clock_ms+=9000;
 return n+2;
}
int btlink_chan_is_open(const btlink *l,unsigned c){(void)l;(void)c;return 1;}
int btlink_is_up(const btlink *l){(void)l;return 1;}
int btlink_pump(btlink *l,int ms){(void)l;clock_ms+=ms;return 1;}
unsigned btlink_chan_open(btlink *l,unsigned p,int ms){(void)l;(void)p;(void)ms;assert(0);return 0;}
void btlink_chan_close(btlink *l,unsigned c){(void)l;(void)c;assert(0);}
unsigned btlink_chan_find_inbound(const btlink *l,unsigned p){(void)l;(void)p;return 0;}
unsigned btlink_chan_find_inbound_other(const btlink *l,unsigned p,unsigned c){(void)l;(void)p;(void)c;return 0;}
void btlink_close_inbound_sdp(btlink *l){(void)l;assert(0);}
void btlink_set_rx(btlink *l,unsigned c,btlink_rx_fn fn,void *u){(void)l;(void)c;(void)fn;(void)u;}
void btlink_set_inbound_rx(btlink *l,unsigned c,btlink_rx_fn fn,void *u){(void)l;(void)c;(void)fn;(void)u;}
int btlink_l2_send_media(btlink *l,unsigned c,const unsigned char *d,int n){(void)l;(void)c;(void)d;(void)n;assert(0);return 0;}
static void run(int mode){
 avdtp_session s;memset(&s,0,sizeof s);s.link=(btlink *)1;s.sig_scid=0x40;
 s.quick=7;s.sink.seid=5;s.sbc_seid=5;s.codec.codec=1;s.codec.avail=2;s.codec.name="SBC";s.configured=1;
 s.sbc_cfg[0]=0x11;s.remote_cfg=1;s.remote_seid=5;
 avdtp_session before=s;scenario=mode;clock_ms=10000;tx_count=fallbacks=0;logs[0]=discovery[0]=0;
 diagnose_sink_codecs(&s);
 assert(s.sink.seid==before.sink.seid&&s.sbc_seid==before.sbc_seid&&s.remote_seid==before.remote_seid);
 assert(!memcmp(&s.codec,&before.codec,sizeof s.codec)&&!memcmp(s.sbc_cfg,before.sbc_cfg,4));
 assert(s.configured==1&&s.remote_cfg==1&&s.quick==7&&!s.diag_query&&!s.in_cmd);
 if(mode==1||mode==2){assert(strncmp(discovery,"complete:",9)==0);assert(strstr(discovery,"AAC 1"));assert(strstr(logs,"vendor_id=0x4f"));assert(tx_count==(mode==1?4:5));}
 else {assert(strncmp(discovery,"incomplete:",11)==0||strncmp(discovery,"INCOMPLETE:",11)==0);assert(strstr(discovery,"AAC unknown"));}
 if(mode==2)assert(fallbacks==1);
 if(mode==6)assert(tx_count==2); /* budget stops further probes */
}
int main(void){for(int s=1;s<=7;s++)run(s);puts("PASS: 7 AVDTP diagnostic scans; codec/config preserved, in-use AAC, fallback, timeout, malformed, fragmented, budget");return 0;}
