#include "caps_diag.h"
#include <string.h>
const char *hb_caps_diag_name(unsigned codec)
{
    switch(codec) { case 0: return "SBC"; case 1: return "MPEG1/2";
    case 2: return "AAC"; case 4: return "ATRAC"; case 255: return "vendor";
    default: return "unknown"; }
}
int hb_caps_diag_parse(const unsigned char *data, int len, hb_caps_diag *out)
{
    int off=0;
    if (!out) return 0;
    memset(out,0,sizeof *out);
    if (!data || len < 0) return 0;
    while (off<len) {
        int n;
        if (len-off<2) return 0;
        n=data[off+1];
        if (n>len-off-2) return 0;
        if (data[off]==7) {
            hb_diag_codec *c;
            if (n<2 || out->count>=HB_DIAG_MAX_CODECS) return 0;
            c=&out->codecs[out->count++];
            c->media=data[off+2]>>4; c->codec=data[off+3];
            c->ie_offset=off+4; c->ie_len=n-2;
            if(c->media==0 && ((c->codec==0 && c->ie_len<4) ||
               (c->codec==2 && c->ie_len<6) || (c->codec==255 && c->ie_len<6))) return 0;
        } else if (data[off]==8) out->delay_report=1;
        off+=n+2;
    }
    out->valid=1;
    return 1;
}
