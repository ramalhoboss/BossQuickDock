#ifndef HB_STREAM_DIAG_H
#define HB_STREAM_DIAG_H
#include <stdint.h>
typedef struct { uint64_t calls,total_us,max_us,invalid; } hb_diag_time;
static inline void hb_diag_time_add(hb_diag_time *s,uint64_t begin,uint64_t end)
{
    uint64_t d;
    if (!begin || end<begin) { s->invalid++; return; }
    d=end-begin; s->calls++; s->total_us+=d;
    if(d>s->max_us)s->max_us=d;
}
static inline uint64_t hb_diag_time_avg(const hb_diag_time *s)
{ return s->calls?s->total_us/s->calls:0; }
typedef struct {
    hb_diag_time capture,encode,submit,assembly,delivery,work;
    uint64_t began_us,last_delivery_us,pcm_frames,empty,hard,submit_failed;
} hb_stream_diag;
#endif
