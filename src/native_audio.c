/* SPDX-License-Identifier: GPL-3.0-or-later
 * BossQuickDock native capture probe. Uses the existing Avcap2 API only.
 * No direct kernel addresses, Bluetooth commands or privilege changes.
 * Compile NATIVE_FORMAT=1 for AAC, 2 for Opus (480-frame configuration).
 * Experimental: ABI inferred from the supplied firmware 13.60 dump. */
#include "native_audio.h"
#include "opus_wire.h"
#include "dynmod.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include "native_mux.h"
#include "capture_guard.h"
#include "log.h"
#include <pthread.h>
#include <fcntl.h>
#include <ps5/kernel.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static pthread_mutex_t n_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t n_thread;
static int n_joinable,n_active,n_cancel,n_format=1,n_seconds=12,n_ready,n_error;
static int n_live,n_port=18098,n_socket=-1;static char n_ip[32];static struct sockaddr_in n_dest;
static uint64_t n_sent,n_send_errors,n_session,n_live_began,n_deadline,n_last_reopen;
static uint64_t n_base_packets,n_base_bytes,n_base_empty,n_base_overrun,n_base_jumps;
static unsigned n_capture_retries,n_network_reopens,n_fail_streak;
static uint64_t clock_us(void);
static uint32_t n_sequence;
typedef ssize_t (*sendto_fn)(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
static sendto_fn n_sendto;
static char n_folder[192],n_reason[160]="Ready",n_export[240];
static uint64_t n_packets,n_bytes,n_empty,n_overrun,n_jumps,n_elapsed;
static int cancelled(void){int v;pthread_mutex_lock(&n_lock);v=n_cancel;pthread_mutex_unlock(&n_lock);return v;}
static void progress(uint64_t p,uint64_t b,uint64_t e,uint64_t o,uint64_t j,uint64_t t){pthread_mutex_lock(&n_lock);n_packets=p+(n_live?n_base_packets:0);n_bytes=b+(n_live?n_base_bytes:0);n_empty=e+(n_live?n_base_empty:0);n_overrun=o+(n_live?n_base_overrun:0);n_jumps=j+(n_live?n_base_jumps:0);n_elapsed=n_live?(clock_us()-n_live_began)/1000:t/1000;pthread_mutex_unlock(&n_lock);}

#ifndef NATIVE_FORMAT
#define NATIVE_FORMAT n_format
#endif
#ifndef PROBE_SECONDS
#define PROBE_SECONDS n_seconds
#endif
#ifndef PROBE_ROOT
#define PROBE_ROOT "/data/hearbridge"
#endif
#define CAP_BYTES 8192
#define MAX_FILE_BYTES (4u*1024u*1024u)
#define EMPTY 0x81950002u
#define OVERRUN 0x81950004u
typedef int (*load_fn)(const char *,size_t,const void *,uint32_t,void *,int *);
typedef int (*init_fn)(void);
typedef int (*open_fn)(int64_t *,void *,int);
typedef int (*handle_fn)(int64_t);
typedef int (*read_fn)(int64_t,void *,size_t,void *,int);
static long long epoch_seconds(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t))return 0;return (long long)t.tv_sec;}
static uint64_t clock_us(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;}
static void put32(unsigned char *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static void put64(unsigned char *p,uint64_t v){for(int i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i));}
static int module(const char *name,load_fn load,uint32_t *h){char p[160];int result=-1;if(!kernel_dynlib_handle(-1,name,h)&&*h)return 0;if(!load)return -1;snprintf(p,sizeof p,"/system/common/lib/%s",name);int rv=load(p,0,NULL,0,NULL,&result);(void)rv;return kernel_dynlib_handle(-1,name,h)||!*h?-1:0;}
static void parameters(unsigned char p[56]){memset(p,0,56);put64(p,56);put64(p+8,24);put32(p+0x14,2);put32(p+0x18,NATIVE_FORMAT);put32(p+0x1c,0xffffffffu);put32(p+0x28,128000);if(NATIVE_FORMAT==2)put32(p+0x30,1);}
static int capture_run(void){
 const char *codec=NATIVE_FORMAT==1?"aac":"opus";uint32_t kh=0,ah=0;load_fn load=NULL;
 init_fn init=NULL,term=NULL;open_fn open_audio=NULL;handle_fn start=NULL,stop=NULL,close_audio=NULL;read_fn read_audio=NULL;
 int initialized=0,started=0,rv=-1,last=0,exit_code=1;int64_t handle=0;
 unsigned char param[56],buf[CAP_BYTES],header[32],record[32];uint64_t meta[2];
 uint64_t began=0,last_data=0,previous_pts=0,packets=0,payload_bytes=0,empty=0,overrun=0,pts_jumps=0;
 int have_pts=0;uint64_t expected_ticks=(NATIVE_FORMAT==1?1024u:480u)*5u;
 char folder[192]={0},data_path[240],manifest_path[240];const char *reason="initialization failed";FILE *data=NULL,*manifest=NULL;
 const char *kernels[]={"libkernel_web.sprx","libkernel.sprx","libkernel_sys.sprx",NULL};
 if(n_live)goto modules;
 mkdir(PROBE_ROOT,0755);
 snprintf(folder,sizeof folder,PROBE_ROOT "/native-%s-%lld-%d",codec,epoch_seconds(),(int)getpid());
 if(mkdir(folder,0755)!=0){pthread_mutex_lock(&n_lock);snprintf(n_reason,sizeof n_reason,"Cannot create recording directory");pthread_mutex_unlock(&n_lock);log_line("native-probe: cannot create new directory (%d)\n",errno);return 1;}
 snprintf(data_path,sizeof data_path,"%s/packets.bqn",folder);snprintf(manifest_path,sizeof manifest_path,"%s/result.json",folder);
 pthread_mutex_lock(&n_lock);snprintf(n_folder,sizeof n_folder,"%s",folder);pthread_mutex_unlock(&n_lock);
 log_line("native: %s recording %d s, output %s",codec,n_seconds,folder);
modules:
 for(int i=0;kernels[i];i++)if(!kernel_dynlib_handle(-1,kernels[i],&kh)&&kh)break;
 if(!kh){reason="libkernel unavailable";goto done;}
 load=(load_fn)kernel_dynlib_resolve(-1,kh,"wzvqT4UqKX8");
 if(module("libSceIpmi.sprx",load,&ah)||module("libSceAvcap2.sprx",load,&ah)){reason="capture modules unavailable";goto done;}
 init=(init_fn)kernel_dynlib_resolve(-1,ah,"svzPXluOz8U");term=(init_fn)kernel_dynlib_resolve(-1,ah,"gojqghDU+1Y");
 open_audio=(open_fn)kernel_dynlib_resolve(-1,ah,"3nHpE7Dp5SE");close_audio=(handle_fn)kernel_dynlib_resolve(-1,ah,"tR7gPe1i8hw");
 start=(handle_fn)kernel_dynlib_resolve(-1,ah,"8CGNCwBsItI");stop=(handle_fn)kernel_dynlib_resolve(-1,ah,"Z5dKK0xnQ8g");read_audio=(read_fn)kernel_dynlib_resolve(-1,ah,"WhsHggqdwcg");
 if(!init||!term||!open_audio||!close_audio||!start||!stop||!read_audio){reason="required capture functions unavailable";goto done;}
 rv=init();last=rv;if(rv<0){reason="Initialize rejected";goto done;}initialized=1;
 parameters(param);rv=open_audio(&handle,param,0);last=rv;if(rv<0||!handle){reason="OpenAudio rejected inferred encoded format";goto done;}
 rv=start(handle);last=rv;if(rv<0){reason="Start rejected";goto done;}started=1;pthread_mutex_lock(&n_lock);snprintf(n_reason,sizeof n_reason,"%s",n_live?"Live Opus active":"Recording");pthread_mutex_unlock(&n_lock);
 if(n_live)goto capture_loop;
 data=fopen(data_path,"wb");if(!data){reason="cannot create packet file";goto done;}
 memset(header,0,sizeof header);memcpy(header,"BQDNCP01",8);put32(header+8,1);put32(header+12,NATIVE_FORMAT);put32(header+16,48000);put32(header+20,2);put32(header+24,NATIVE_FORMAT==1?1024:480);put32(header+28,240000);
 if(fwrite(header,1,32,data)!=32){reason="packet header write failed";goto done;}
capture_loop:
 began=clock_us();last_data=began;if(!began){reason="monotonic clock unavailable";goto done;}
 while(n_live?clock_us()<n_deadline:clock_us()-began<PROBE_SECONDS*1000000ULL){
  if(cancelled()){reason="Stopped by user";exit_code=packets?0:1;goto done;}
  meta[0]=16;meta[1]=0;rv=read_audio(handle,buf,sizeof buf,meta,1);last=rv;
  uint64_t now=clock_us();
  if(rv>0){
   if(rv>CAP_BYTES){reason="invalid positive ReadAudio length";goto done;}
   if(!n_live&&payload_bytes+(uint64_t)rv>MAX_FILE_BYTES){reason="capture file limit reached";goto done;}
   if(n_live){
    unsigned char wire[1248];int len=opus_wire_packet(wire,sizeof wire,n_session,n_sequence++,meta[1],now-began,buf,(size_t)rv);
    ssize_t sent=len>0?n_sendto(n_socket,wire,(size_t)len,MSG_DONTWAIT,(const struct sockaddr *)&n_dest,sizeof n_dest):-1;
    pthread_mutex_lock(&n_lock);if(sent==len&&len>0){n_sent++;n_fail_streak=0;}else{n_send_errors++;n_fail_streak++;}pthread_mutex_unlock(&n_lock);
    if(n_fail_streak>=100&&now-n_last_reopen>=1000000){
     if(n_socket>=0)close(n_socket);n_socket=socket(AF_INET,SOCK_DGRAM,0);n_last_reopen=now;
     pthread_mutex_lock(&n_lock);n_network_reopens++;n_fail_streak=0;pthread_mutex_unlock(&n_lock);
     log_line("native: UDP socket reopen after repeated send errors (%s:%d)",n_ip,n_port);
    }
   }else{
    memcpy(record,meta,16);put64(record+16,now-began);put32(record+24,(uint32_t)rv);put32(record+28,0);
    if(fwrite(record,1,32,data)!=32||fwrite(buf,1,(size_t)rv,data)!=(size_t)rv){reason="packet write failed";goto done;}
   }
   if(meta[0]==16&&meta[1]){if(have_pts&&(meta[1]<previous_pts||meta[1]-previous_pts!=expected_ticks))pts_jumps++;previous_pts=meta[1];have_pts=1;}
   packets++;payload_bytes+=(uint64_t)rv;last_data=now;
  }else if(rv==0||(uint32_t)rv==EMPTY){empty++;}
  else if((uint32_t)rv==OVERRUN){overrun++;}
  else {reason="ReadAudio rejected";goto done;}
  if(now-last_data>5000000ULL){reason="no encoded payload for five seconds";goto done;}
  progress(packets,payload_bytes,empty,overrun,pts_jumps,now-began);
  usleep(1000);
 }
 reason=packets?"Capture complete":"Completed without payload";exit_code=packets?0:1;
done:
 if(data){if(fflush(data)!=0){reason="packet flush failed";exit_code=1;}if(fclose(data)!=0){reason="packet close failed";exit_code=1;}data=NULL;}
 int stop_rv=0,close_rv=0,term_rv=0;
 if(started&&handle)stop_rv=stop(handle);if(handle&&close_audio)close_rv=close_audio(handle);if(initialized&&term)term_rv=term();
 if(stop_rv||close_rv||term_rv){reason="Native capture cleanup returned an error";exit_code=1;}
 if(n_live)goto finished;
 manifest=fopen(manifest_path,"wb");if(manifest){fprintf(manifest,"{\n  \"probe\":\"BossQuickDock-native-capture-1\",\n  \"codec_candidate\":\"%s\",\n  \"format_field\":%d,\n  \"sample_rate\":48000,\n  \"channels\":2,\n  \"bitrate_requested\":128000,\n  \"frames_per_record_expected\":%d,\n  \"pts_timebase_candidate\":240000,\n  \"packets\":%llu,\n  \"payload_bytes\":%llu,\n  \"empty_reads\":%llu,\n  \"overruns\":%llu,\n  \"pts_discontinuities\":%llu,\n  \"last_return_hex\":\"0x%08x\",\n  \"stop_return\":%d,\n  \"close_return\":%d,\n  \"terminate_return\":%d,\n  \"result\":\"%s\",\n  \"codec_validated\":false,\n  \"note\":\"Read-only capture session; encoded format inferred from firmware dump. Payload must be parsed and decoded before claiming success.\"\n}\n",codec,NATIVE_FORMAT,NATIVE_FORMAT==1?1024:480,(unsigned long long)packets,(unsigned long long)payload_bytes,(unsigned long long)empty,(unsigned long long)overrun,(unsigned long long)pts_jumps,(unsigned)last,stop_rv,close_rv,term_rv,reason);fclose(manifest);}else exit_code=1;
finished:
 log_line("native-probe: %s; packets=%llu bytes=%llu; output=%s\n",reason,(unsigned long long)packets,(unsigned long long)payload_bytes,folder);
 progress(packets,payload_bytes,empty,overrun,pts_jumps,began?clock_us()-began:0);
 pthread_mutex_lock(&n_lock);snprintf(n_reason,sizeof n_reason,"%s",reason);pthread_mutex_unlock(&n_lock);
 return exit_code;
}

static void *record_worker(void *arg){
 char input[240],output[240];int rc;(void)arg;
 if(n_live){
  unsigned char ip[4];memset(&n_dest,0,sizeof n_dest);n_dest.sin_family=AF_INET;n_dest.sin_port=htons((unsigned short)n_port);opus_wire_ipv4(n_ip,ip);memcpy(&n_dest.sin_addr,ip,4);
  n_sendto=(sendto_fn)hb_kernel_sym(NULL,"sendto",NULL);
  if(!n_sendto){uint32_t mh=hb_mod_open("libSceNet.sprx",NULL,0);n_sendto=(sendto_fn)hb_mod_sym(mh,NULL,"sendto");}
  n_socket=n_sendto?socket(AF_INET,SOCK_DGRAM,0):-1;
  if(n_socket<0){pthread_mutex_lock(&n_lock);snprintf(n_reason,sizeof n_reason,"UDP sender unavailable");n_error=1;capture_guard_release(2);n_active=0;pthread_mutex_unlock(&n_lock);return NULL;}
  log_line("native: live Opus UDP to %s:%d",n_ip,n_port);
 }
 if(n_live){n_live_began=clock_us();n_deadline=n_live_began+7200ULL*1000000;n_last_reopen=n_live_began;}
 for(;;){
  rc=capture_run();
  if(!n_live||!rc||cancelled()||clock_us()>=n_deadline||n_capture_retries>=3)break;
  pthread_mutex_lock(&n_lock);n_capture_retries++;n_base_packets=n_packets;n_base_bytes=n_bytes;n_base_empty=n_empty;n_base_overrun=n_overrun;n_base_jumps=n_jumps;snprintf(n_reason,sizeof n_reason,"Recovering native capture (%u/3)",n_capture_retries);pthread_mutex_unlock(&n_lock);
  log_line("native: live capture recovery %u/3",n_capture_retries);
  for(int wait=0;wait<10&&!cancelled();wait++)usleep(100000);
  if(cancelled())break;
  n_session++;n_sequence=0;
 }
 if(n_live){
  if(n_socket>=0){unsigned char end[48];opus_wire_packet(end,sizeof end,n_session,n_sequence,0,0,NULL,0);n_sendto(n_socket,end,sizeof end,MSG_DONTWAIT,(const struct sockaddr *)&n_dest,sizeof n_dest);close(n_socket);}n_socket=-1;
  pthread_mutex_lock(&n_lock);n_ready=0;n_error=rc!=0;capture_guard_release(2);n_active=0;pthread_mutex_unlock(&n_lock);
  log_line("native: live ended; sent=%llu send-errors=%llu",(unsigned long long)n_sent,(unsigned long long)n_send_errors);return NULL;
 }
 snprintf(input,sizeof input,"%s/packets.bqn",n_folder);
 snprintf(output,sizeof output,"%s/recording.%s",n_folder,n_format==1?"aac":"opus");
 int mux_rc=n_packets?native_mux(input,output,n_format):-1;
 pthread_mutex_lock(&n_lock);
 n_ready=mux_rc==0;n_error=rc!=0||mux_rc!=0;
 if(n_ready)snprintf(n_export,sizeof n_export,"%s",output);
 if(!rc&&mux_rc)snprintf(n_reason,sizeof n_reason,"Container export failed; raw packets retained");
 capture_guard_release(2);
 n_active=0;
 pthread_mutex_unlock(&n_lock);
 log_line("native: finished format=%d packets=%llu export=%s",n_format,(unsigned long long)n_packets,mux_rc?"failed":"ready");return NULL;
}
static int native_audio_begin(int format,int seconds,const char *ip,int port){
 if((format!=1&&format!=2)||(!ip&&seconds!=12&&seconds!=30&&seconds!=60))return -2;
 pthread_mutex_lock(&n_lock);
 if(n_active){pthread_mutex_unlock(&n_lock);return -1;}
 if(n_joinable){pthread_join(n_thread,NULL);n_joinable=0;}
 if(!capture_guard_take(2)){pthread_mutex_unlock(&n_lock);return -1;}
 n_live=ip!=NULL;n_port=port;snprintf(n_ip,sizeof n_ip,"%s",ip?ip:"");n_sent=n_send_errors=0;n_sequence=0;n_capture_retries=n_network_reopens=n_fail_streak=0;n_base_packets=n_base_bytes=n_base_empty=n_base_overrun=n_base_jumps=0;n_session=((uint64_t)epoch_seconds()<<32)^clock_us();
 n_format=format;n_seconds=seconds;n_cancel=0;n_ready=0;n_error=0;n_packets=n_bytes=n_empty=n_overrun=n_jumps=n_elapsed=0;n_folder[0]=n_export[0]=0;
 snprintf(n_reason,sizeof n_reason,"Starting capture");n_active=1;
 if(pthread_create(&n_thread,NULL,record_worker,NULL)!=0){n_active=0;n_error=1;snprintf(n_reason,sizeof n_reason,"Recorder thread unavailable");capture_guard_release(2);pthread_mutex_unlock(&n_lock);return -3;}
 n_joinable=1;pthread_mutex_unlock(&n_lock);return 0;
}
int native_audio_start(int format,int seconds){return native_audio_begin(format,seconds,NULL,0);}
int native_audio_live(const char *ip,int port){unsigned char parsed[4];if(!opus_wire_ipv4(ip,parsed)||port<1024||port>65535)return -2;return native_audio_begin(2,7200,ip,port);}
void native_audio_cancel(void){pthread_mutex_lock(&n_lock);n_cancel=1;pthread_mutex_unlock(&n_lock);}
int native_audio_busy(void){int b;pthread_mutex_lock(&n_lock);b=n_active;pthread_mutex_unlock(&n_lock);return b;}
void native_audio_shutdown(void){native_audio_cancel();if(n_joinable){pthread_join(n_thread,NULL);n_joinable=0;}}
int native_audio_json(char *out,int max){int n;pthread_mutex_lock(&n_lock);
 n=snprintf(out,(size_t)max,"{\"active\":%s,\"ready\":%s,\"error\":%s,\"codec\":\"%s\",\"duration_s\":%d,\"elapsed_ms\":%llu,\"packets\":%llu,\"payload_bytes\":%llu,\"empty_reads\":%llu,\"overruns\":%llu,\"pts_discontinuities\":%llu,\"frame_ms\":%s,\"reason\":\"%s\",\"folder\":\"%s\",\"decoder_verified_on_console_probe\":true,\"current_recording_decoder_verified\":false,\"end_to_end_latency_measured\":false}",n_active?"true":"false",n_ready?"true":"false",n_error?"true":"false",n_format==1?"AAC":"Opus",n_seconds,(unsigned long long)n_elapsed,(unsigned long long)n_packets,(unsigned long long)n_bytes,(unsigned long long)n_empty,(unsigned long long)n_overrun,(unsigned long long)n_jumps,n_format==1?"21.333":"10",n_reason,n_folder);
 if(n>0&&n<max-1){n--;n+=snprintf(out+n,(size_t)(max-n),",\"mode\":\"%s\",\"destination\":\"%s\",\"port\":%d,\"sent_packets\":%llu,\"send_errors\":%llu}",n_live?"live":"recording",n_ip,n_port,(unsigned long long)n_sent,(unsigned long long)n_send_errors);}
 if(n>0&&n<max-1){n--;n+=snprintf(out+n,(size_t)(max-n),",\"capture_retries\":%u,\"network_reopens\":%u}",n_capture_retries,n_network_reopens);}
 pthread_mutex_unlock(&n_lock);return n;
}
int native_audio_download(int raw,char *name,int max){int fd=-1;char path[240];pthread_mutex_lock(&n_lock);
 if(!n_active&&n_packets&&n_folder[0]&&(raw||n_ready)){
 if(raw)snprintf(path,sizeof path,"%s/packets.bqn",n_folder);else snprintf(path,sizeof path,"%s",n_export);
 snprintf(name,(size_t)max,"BossQuickDock-%s.%s",n_format==1?"AAC":"Opus",raw?"bqn":n_format==1?"aac":"opus");fd=open(path,O_RDONLY);
 }pthread_mutex_unlock(&n_lock);return fd;
}
