/* Regression tests of the real capture reader with a fake API/clock. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#define usleep test_usleep
int test_usleep(unsigned int us);
#include "../src/avcap2.c"
static long clock_ms=10000;
static int read_calls,init_calls,term_calls,close_calls,stop_calls,sleep_calls;
static int read_status,init_status,start_status,open_status;
static unsigned char input[READ_BYTES];static int input_bytes;
/* The library hands over one packet per read, whatever buflen is: 1024
 * frames, 8 KB of stereo float. The buffer (READ_BYTES) is larger. */
#define PACKET_BYTES 0x2000
long now_ms(void){return clock_ms;}
int test_usleep(unsigned int us){sleep_calls++;clock_ms+=us/1000;return 0;}
void log_line(const char *fmt,...){(void)fmt;}
void diag_set(const char *key,const char *fmt,...){(void)key;(void)fmt;}
int diag_save(void){return 0;}
int kernel_dynlib_handle(pid_t p,const char *n,uint32_t *h){(void)p;(void)n;*h=0;return -1;}
intptr_t kernel_dynlib_resolve(pid_t p,uint32_t h,const char *n){(void)p;(void)h;(void)n;return 0;}
uint64_t kernel_get_ucred_authid(pid_t p){(void)p;return 0;}
uint64_t creds_elevate(uint64_t a,hb_creds *s,const char *t){(void)s;(void)t;return a;}
static int fake_init(void){init_calls++;return init_status;}
static int fake_term(void){term_calls++;return 0;}
static int fake_open(int64_t *h,void *p,int f){(void)p;(void)f;if(open_status)return open_status;*h=123;return 0;}
static int fake_close(int64_t h){(void)h;close_calls++;return 0;}
static int fake_stop(int64_t h){(void)h;stop_calls++;return 0;}
static int fake_start(int64_t h){assert(h==123);return start_status;}
static int fake_read(int64_t h,void *b,size_t len,void *meta,int flags){
 (void)h;(void)meta;assert(flags==AVCAP2_READ_NOWAIT&&flags==1);read_calls++;
 if(read_status)return read_status;
 assert(len>=(size_t)input_bytes);memcpy(b,input,(size_t)input_bytes);return input_bytes;
}
static void setup(avcap2_session *s,int s16,int nch){
 memset(s,0,sizeof *s);memset(input,0,sizeof input);clock_ms=10000;
 read_calls=init_calls=term_calls=close_calls=stop_calls=sleep_calls=0;
 read_status=init_status=start_status=open_status=0;input_bytes=PACKET_BYTES;
 s->buf=calloc(1,READ_BYTES);assert(s->buf);
 s->Init=fake_init;s->Term=fake_term;s->Open=fake_open;s->Close=fake_close;
 s->Start=fake_start;s->Stop=fake_stop;s->Read=fake_read;s->handle=123;
 s->nch=nch;s->fmt_s16=s16;s->fmt_checked=1;s->rate_done=1;s->last_ok_ms=clock_ms;s->watchdog_since_ms=clock_ms;
}
static int get(avcap2_session *s){int16_t out[2048];float peak=99;int n=avcap2_session_read_s16(s,out,1024,&peak);assert(peak>=0.f&&peak<=1.f);assert(!sleep_calls);return n;}
static void test_s16_pending(int cap){
 avcap2_session s;int16_t out[2050];int delivered=0;float peak;setup(&s,1,2);
 int16_t *src=(int16_t *)(void *)input;
 for(int i=0;i<PACKET_BYTES/2;i++)src[i]=(int16_t)(i*7-14000);
 while(delivered<2048){
  for(int j=0;j<2050;j++)out[j]=12345;
  int n=avcap2_session_read_s16(&s,out,cap,&peak);assert(n>0&&n<=cap&&read_calls==1);
  for(int i=0;i<n*2;i++)assert(out[i]==(int16_t)(((float)src[delivered*2+i]/32768.f)*32767.f));
  assert(out[n*2]==12345&&peak>=0.f&&peak<=1.f);delivered+=n;
 }
 assert(delivered==2048&&!s.pending_frames&&!s.pending_offset);free(s.buf);
}
static void test_float_pending(int nch){
 avcap2_session s;int16_t out[130];int delivered=0;float peak;setup(&s,0,nch);
 float *src=(float *)(void *)input;for(int i=0;i<PACKET_BYTES/4;i++)src[i]=(float)((i%101)-50)/100.f;
 int total=PACKET_BYTES/(nch*4);
 while(delivered<total){int n=avcap2_session_read_s16(&s,out,63,&peak);assert(n>0&&n<=63&&read_calls==1);
  for(int f=0;f<n;f++)for(int c=0;c<2;c++)assert(out[f*2+c]==(int16_t)(src[(delivered+f)*nch+c]*32767.f));
  delivered+=n;s.nch=2;
 }assert(delivered==total&&!s.pending_frames);free(s.buf);
}
static void test_unmapped(void){
 avcap2_session s;setup(&s,1,2);read_status=(int)AVCAP2_UNMAPPED;
 assert(get(&s)==0&&!s.recovering&&s.recoveries==1&&s.reads_unmapped==1&&init_calls==1);
 assert(stop_calls==1&&close_calls==1&&term_calls==1);
 assert(!s.pending_frames&&s.nch==2&&!s.fmt_checked&&!s.rate_done);
 read_status=0;assert(get(&s)==1024&&read_calls==2);free(s.buf);
}
static void test_recovery_failure(void){
 avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_UNMAPPED;init_status=-7;
 assert(get(&s)==0&&s.recovering&&s.recoveries==1);
 for(int i=0;i<100;i++)assert(get(&s)==0);
 assert(s.recoveries==1&&read_calls==1);clock_ms+=1000;assert(get(&s)==0);
 clock_ms+=1000;assert(get(&s)==-1&&s.failed&&s.recoveries==3&&init_calls==6);
 int calls=init_calls;assert(get(&s)==-1&&calls==init_calls);free(s.buf);
}
static void test_repeated_unmapped(void){
 avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_UNMAPPED;
 for(int i=0;i<3;i++){assert(get(&s)==0&&!s.recovering&&s.recoveries==i+1);clock_ms+=1000;}
 assert(get(&s)==-1&&s.failed&&init_calls==3);free(s.buf);
}
static void test_empty_transient(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_EMPTY;clock_ms+=300;assert(get(&s)==0&&!s.recoveries&&s.reads_empty==1);free(s.buf);}
static void test_empty_watchdog(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_EMPTY;clock_ms+=4999;assert(get(&s)==0&&!s.recoveries);clock_ms++;assert(get(&s)==0&&s.recoveries==1&&s.no_data_events==1);assert(s.last_ok_ms==10000&&s.watchdog_since_ms==clock_ms&&s.awaiting_pcm);free(s.buf);}
static void test_zero_pcm_healthy(void){avcap2_session s;setup(&s,0,2);for(int i=0;i<120;i++){clock_ms+=500;assert(get(&s)==1024);}assert(!s.recoveries&&!s.no_data_events&&!s.reads_empty);free(s.buf);}
static void test_invalid_transient(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_INVALID;assert(get(&s)==0);clock_ms+=100;read_status=0;assert(get(&s)==1024&&!s.recoveries&&!s.problem_active);free(s.buf);}
static void test_persistent_problem(uint32_t code){avcap2_session s;setup(&s,0,2);read_status=(int)code;assert(get(&s)==0&&!s.recoveries);clock_ms+=999;assert(get(&s)==0&&!s.recoveries);clock_ms++;assert(get(&s)==0&&s.recoveries==1&&!s.recovering);free(s.buf);}
static void test_overrun_transient(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_OVERRUN;assert(get(&s)==0&&s.reads_overrun==1&&!s.reads_empty&&!s.recoveries);read_status=0;assert(get(&s)==1024);free(s.buf);}
static void test_overrun_watchdog(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_OVERRUN;clock_ms+=5000;assert(get(&s)==0&&s.recoveries==1&&s.no_data_events==1);assert(s.last_ok_ms==10000&&s.watchdog_since_ms==clock_ms&&s.awaiting_pcm);free(s.buf);}
static void test_zero_length(void){avcap2_session s;setup(&s,0,2);input_bytes=0;assert(get(&s)==0&&s.reads_zero==1);clock_ms+=5000;assert(get(&s)==0&&s.recoveries==1);free(s.buf);}
static void test_short_pcm(void){avcap2_session s;setup(&s,0,2);input_bytes=3;assert(get(&s)==0&&!s.reads_ok);clock_ms+=1000;assert(get(&s)==0&&s.recoveries==1);free(s.buf);}
static void test_oversize(void){avcap2_session s;setup(&s,0,2);read_status=READ_BYTES+64;assert(get(&s)==0&&!s.reads_ok);clock_ms+=1000;assert(get(&s)==0&&s.recoveries==1);free(s.buf);}
static void test_budget_reset(void){
 avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_UNMAPPED;assert(get(&s)==0&&s.recoveries==1);
 read_status=0;assert(get(&s)==1024);clock_ms+=30000;assert(get(&s)==1024&&!s.recoveries&&s.recoveries_total==1);
 read_status=(int)AVCAP2_UNMAPPED;assert(get(&s)==0&&s.recoveries==1&&s.recoveries_total==2);free(s.buf);
}
static void test_no_budget_reset_without_pcm(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_EMPTY;for(int i=0;i<3;i++){clock_ms+=5000;assert(get(&s)==0&&s.recoveries==i+1);}clock_ms+=5000;assert(get(&s)==-1&&s.failed&&s.recoveries_total==3&&init_calls==3);free(s.buf);}
static void test_start_failure(void){avcap2_session s;setup(&s,0,2);read_status=(int)AVCAP2_UNMAPPED;start_status=-9;assert(get(&s)==0&&!s.handle&&s.recovering);assert(close_calls==2);clock_ms+=1000;assert(get(&s)==0);clock_ms+=1000;assert(get(&s)==-1&&s.failed&&!s.handle);free(s.buf);}
/* A 1024-frame packet of 12-channel float (48 KB) fits the buffer; with the
 * old 8 KB buffer the library answered 0x81950001 on every read. */
static void test_big_packet(void){avcap2_session s;setup(&s,0,12);input_bytes=1024*12*4;assert(get(&s)==1024&&s.reads_ok==1&&!s.pending_frames&&!s.reads_invalid);free(s.buf);}
static void test_pending_before_new_read(void){avcap2_session s;setup(&s,1,2);assert(get(&s)==1024&&s.pending_frames==1024);read_status=(int)AVCAP2_UNMAPPED;assert(get(&s)==1024&&read_calls==1&&!s.recoveries);assert(get(&s)==0&&read_calls==2&&s.recoveries==1);free(s.buf);}
static void test_invalid_arguments(void){avcap2_session s;float peak;int16_t out[2];setup(&s,0,2);assert(avcap2_session_read_s16(0,out,1,&peak)==-1);assert(avcap2_session_read_s16(&s,0,1,&peak)==-1);assert(avcap2_session_read_s16(&s,out,0,&peak)==-1);assert(!read_calls);free(s.buf);}
static void test_stats_snapshot(void){
 avcap2_session s;avcap2_stats a;setup(&s,0,2);assert(get(&s)==1024);clock_ms+=42;
 avcap2_session_stats(&s,&a);assert(a.state==1&&a.age_ms==42&&a.valid==1&&a.empty==0);
 s.recovering=1;s.recoveries_total=2;s.reads_overrun=3;s.last_error=AVCAP2_UNMAPPED;
 avcap2_session_stats(&s,&a);assert(a.state==3&&a.recoveries==2&&a.overrun==3&&a.last_error==AVCAP2_UNMAPPED);
 s.failed=1;avcap2_session_stats(&s,&a);assert(a.state==4);
 avcap2_session_stats(NULL,&a);assert(!a.state&&!a.valid&&!a.recoveries);avcap2_session_stats(&s,NULL);free(s.buf);
}

static int latest_calls,latest_count;
static int latest_read(int64_t h,void *b,size_t len,void *meta,int flags){(void)h;(void)meta;assert(len==READ_BYTES&&flags==1);if(latest_calls>=latest_count){latest_calls++;return (int)AVCAP2_EMPTY;}latest_calls++;float *f=b;for(int i=0;i<PACKET_BYTES/4;i++)f[i]=(float)latest_calls/128.f;return PACKET_BYTES;}
static void test_latest_live_edge(void){avcap2_session s;int16_t out[2048];float peak;unsigned skipped;setup(&s,0,2);s.Read=latest_read;latest_calls=0;latest_count=3;assert(avcap2_session_read_latest_s16(&s,out,1024,&peak,&skipped)==1024);assert(skipped==2048&&latest_calls==4&&out[0]==(int16_t)((3.f/128.f)*32767.f));assert(peak==3.f/128.f);free(s.buf);setup(&s,0,2);s.Read=latest_read;latest_calls=0;latest_count=100;assert(avcap2_session_read_latest_s16(&s,out,1024,&peak,&skipped)==1024);assert(latest_calls==16&&skipped==15*1024);free(s.buf);puts("PASS fresh capture: newest block, accurate skipped frames, bounded 16-read drain");}

int main(void){
 test_latest_live_edge();
 test_stats_snapshot();
 test_s16_pending(1024);test_s16_pending(37);test_s16_pending(1);test_float_pending(2);test_float_pending(8);
 test_unmapped();test_recovery_failure();test_repeated_unmapped();test_empty_transient();test_empty_watchdog();
 test_zero_pcm_healthy();test_invalid_transient();test_persistent_problem(AVCAP2_INVALID);
 test_overrun_transient();test_overrun_watchdog();test_persistent_problem(0x81950008u);test_persistent_problem(0x8195007fu);
 test_zero_length();test_short_pcm();test_oversize();test_budget_reset();test_no_budget_reset_without_pcm();
 test_start_failure();test_pending_before_new_read();test_invalid_arguments();test_big_packet();
 puts("PASS: 27 capture scenarios; every native read used flag 1; no recovery sleeps");return 0;
}
