#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../src/a2dp/btlink.c"
static long test_clock=10000;static int sent;
long now_ms(void){return test_clock;}void log_line(const char *s,...){(void)s;}
static int output(void *self,const unsigned char *p,int n){(void)self;(void)p;assert(n>8);sent++;return 1;}
int main(void){btlink l;memset(&l,0,sizeof l);hci_ops ops={0};ops.acl_send=output;l.hci.ops=&ops;l.connected=1;l.handle=2;l.acl_mtu=1021;l.pool.limit=7;l.pool.last_stats_ms=test_clock;l.pool.last_credit_ms=test_clock;unsigned char p[5]={1,2,3,4,5};
 btlink_set_media_direct(&l,1);btlink_set_media_cap(&l,64);assert(l.media_cap==3);btlink_set_media_pace(&l,11);assert(l.media_dur_ms==0);l.tx_is_media=1;l.media_clock=test_clock+10000;assert(l2_send_raw(&l,64,p,5)&&sent==1&&!l.txq_n);
 l.pool.outstanding=3;int before=sent;assert(l2_send_raw(&l,64,p,5));assert(sent==before&&l.txq_media==1);l.pool.outstanding=2;tx_flush(&l);assert(sent==before+1&&!l.txq_n);sent=1;
 l.pool.outstanding=l.pool.limit;for(int i=0;i<5;i++){test_clock++;assert(l2_send_raw(&l,64,p,5));}assert(l.txq_media==3&&l.pool.media_dropped==2);test_clock+=24;tx_flush(&l);assert(!l.txq_n&&l.pool.media_dropped==5&&sent==1);
 l.tx_is_media=0;assert(l2_send_raw(&l,1,p,5));test_clock+=25;tx_flush(&l);assert(l.txq_n==1&&!l.txq_media);l.pool.outstanding=0;tx_flush(&l);assert(sent==2&&!l.txq_n);
 btlink_set_media_direct(&l,0);btlink_set_media_cap(&l,16);btlink_set_media_pace(&l,10);assert(l.media_cap==16&&l.media_dur_ms==10);l.tx_is_media=1;l.media_clock=test_clock+50;assert(l2_send_raw(&l,64,p,5));assert(l.txq_n==1&&sent==2);test_clock+=50;tx_flush(&l);assert(sent==3&&!l.txq_n);puts("PASS real btlink queue: immediate PCM-clocked send, cap 3, expiry at 24 ms without credits, signaling preserved, legacy pacing preserved");return 0;}
