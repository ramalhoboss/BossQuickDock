/* Host test of bounded probe lifecycle and exact inferred format parameters. */
#define clock_gettime fake_clock_gettime
#define usleep fake_usleep
#include "../src/native_audio.c"

#include <assert.h>
#include <stdlib.h>
static int mode,init_n,open_n,start_n,stop_n,close_n,term_n,read_n;static uint64_t fake_us;
int fake_clock_gettime(clockid_t clock,struct timespec *t){(void)clock;fake_us+=10000;t->tv_sec=(time_t)(fake_us/1000000);t->tv_nsec=(long)(fake_us%1000000)*1000;return 0;}
int fake_usleep(unsigned int n){(void)n;return 0;}
static int fake_init(void){init_n++;return mode==5?-1:0;}
static int fake_term(void){term_n++;return 0;}
static int fake_open(int64_t *h,void *param,int flags){unsigned char *p=param;uint32_t v;assert(flags==0);memcpy(&v,p+0x18,4);assert(v==NATIVE_FORMAT);memcpy(&v,p+0x28,4);assert(v==128000);memcpy(&v,p+0x14,4);assert(v==2);if(NATIVE_FORMAT==2){memcpy(&v,p+0x30,4);assert(v==1);}open_n++;if(mode==3)return (int)0x81950001u;*h=123;return 0;}
static int fake_start(int64_t h){assert(h==123);start_n++;return mode==4?-1:0;}
static int fake_stop(int64_t h){assert(h==123);stop_n++;return 0;}
static int fake_close(int64_t h){assert(h==123);close_n++;return 0;}
static int fake_read(int64_t h,void *b,size_t n,void *meta,int flag){assert(h==123&&n==8192&&flag==1);read_n++;if((mode==13&&read_n==1)||mode==14||(mode==16&&read_n==6))return (int)0x81950003u;if((mode==13&&read_n==4)||(mode==15&&read_n==130)||(mode==16&&read_n==10))native_audio_cancel();if((mode==7||mode==10||mode==11)&&read_n==3)native_audio_cancel();if(mode==1)return (int)EMPTY;if(mode==2)return (int)0x81950003u;if(mode==6&&read_n<3)return (int)OVERRUN;memset(b,0xaa,37);if(NATIVE_FORMAT==2)((unsigned char *)b)[0]=0xf4;uint64_t *m=meta;m[0]=16;m[1]=100000+(uint64_t)read_n*(NATIVE_FORMAT==1?5120:2400);return 37;}
int kernel_dynlib_handle(pid_t p,const char *name,uint32_t *h){(void)p;(void)name;*h=1;return 0;}
intptr_t kernel_dynlib_resolve(pid_t p,uint32_t h,const char *name){(void)p;(void)h;
 if(!strcmp(name,"svzPXluOz8U"))return (intptr_t)fake_init;if(!strcmp(name,"gojqghDU+1Y"))return (intptr_t)fake_term;
 if(!strcmp(name,"3nHpE7Dp5SE"))return (intptr_t)fake_open;if(!strcmp(name,"tR7gPe1i8hw"))return (intptr_t)fake_close;
 if(!strcmp(name,"8CGNCwBsItI"))return (intptr_t)fake_start;if(!strcmp(name,"Z5dKK0xnQ8g"))return (intptr_t)fake_stop;
 if(!strcmp(name,"WhsHggqdwcg"))return (intptr_t)fake_read;return 0;
}
static ssize_t fake_sendto(int fd,const void *p,size_t n,int flags,const struct sockaddr *a,socklen_t al){(void)fd;(void)a;(void)al;assert(flags==MSG_DONTWAIT&&n>=48&&!memcmp(p,"BQDOPUS1",8));return mode==11||mode==15?-1:(ssize_t)n;}
int socket(int a,int b,int c){(void)a;(void)b;(void)c;return open("NUL",O_RDONLY);}
intptr_t hb_kernel_sym(const char *a,const char *b,uint32_t *c){(void)a;(void)b;(void)c;return mode==10||mode==11||mode>=13?(intptr_t)fake_sendto:0;}uint32_t hb_mod_open(const char *a,char *b,int c){(void)a;(void)b;(void)c;return 0;}intptr_t hb_mod_sym(uint32_t h,const char *a,const char *b){(void)h;(void)a;(void)b;return 0;}
void log_line(const char *fmt,...){(void)fmt;}
int main(int argc,char **argv){assert(argc==3);mode=atoi(argv[1]);n_format=atoi(argv[2]);assert(capture_guard_take(1));assert(native_audio_start(n_format,12)==-1);assert(capture_guard_owner()==1);capture_guard_release(1);assert(capture_guard_owner()==0);assert(native_audio_start(3,12)==-2);if(mode>=10){assert(native_audio_live("192.168.1.134",18098)==0);pthread_join(n_thread,NULL);n_joinable=0;assert(!n_active&&capture_guard_owner()==0&&!n_ready);if(mode==12){assert(n_error&&!init_n);}else if(mode==13){assert(n_capture_retries==1&&init_n==2&&term_n==2&&n_sent==3&&n_packets==3);}else if(mode==14){assert(n_capture_retries==3&&init_n==4&&term_n==4&&n_error);}else if(mode==15){assert(n_network_reopens>=1&&n_send_errors==130&&!n_capture_retries);}else if(mode==16){assert(n_capture_retries==1&&n_packets==9&&n_sent==9&&init_n==2&&term_n==2);}else{assert(read_n==3&&stop_n==1&&close_n==1&&term_n==1);assert(n_sent==(mode==10?3:0));assert(n_send_errors==(mode==11?3:0));}printf("PASS live worker scenario=%d\n",mode);return 0;}if(mode==8||mode==9){assert(native_audio_start(n_format,12)==0);if(mode==8)native_audio_shutdown();else {pthread_join(n_thread,NULL);n_joinable=0;}assert(!native_audio_busy()&&capture_guard_owner()==0);assert(stop_n==1&&close_n==1&&term_n==1);if(mode==9){assert(n_ready);char name[96];int fd=native_audio_download(0,name,sizeof name);assert(fd>=0);close(fd);}printf("PASS async recorder scenario=%d\n",mode);return 0;}int result=capture_run();
 assert(init_n==1);if(mode==5){assert(result==1&&!open_n&&!term_n);}
 else {assert(term_n==1&&open_n==1);if(mode==3){assert(result==1&&!start_n&&!close_n&&!stop_n);}else if(mode==4){assert(result==1&&start_n==1&&close_n==1&&!stop_n);}else {assert(start_n==1&&close_n==1&&stop_n==1);assert(read_n>0);assert(result==((mode==1||mode==2)?1:0));}}
 printf("PASS native-format=%d scenario=%d\n",NATIVE_FORMAT,mode);return 0;}
