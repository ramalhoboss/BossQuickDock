#ifndef HB_CAPS_DIAG_H
#define HB_CAPS_DIAG_H
#include <stdint.h>
#define HB_DIAG_MAX_CODECS 64
typedef struct { unsigned media, codec; int ie_offset, ie_len; } hb_diag_codec;
typedef struct { int valid, delay_report, count; hb_diag_codec codecs[HB_DIAG_MAX_CODECS]; } hb_caps_diag;
/* No selection or transport state is changed. Offsets refer to the input. */
int hb_caps_diag_parse(const unsigned char *data, int len, hb_caps_diag *out);
const char *hb_caps_diag_name(unsigned codec);
#endif
