/* Media encoder for the stream loop: SBC (sbc.h) or aptX (aptx_enc.h)
 * behind one set of calls, so the packer does not care which one runs.
 * aptX has a fixed rate: no bitpool (0), set_bitpool does nothing. */
#ifndef HEARBRIDGE_MENC_H
#define HEARBRIDGE_MENC_H

#include "sbc.h"

#include <stddef.h>
#include <stdint.h>

typedef struct media_enc media_enc;

media_enc *menc_open_sbc(const sbc_config *cfg);
media_enc *menc_open_aptx(void);
void       menc_close(media_enc *m);

int    menc_is_aptx(const media_enc *m);
/* Same contract as sbc_encoder_encode(): bytes written, 0 = needs more, -1 = error. */
int    menc_encode(media_enc *m, const int16_t *pcm, int frames, unsigned char *out, size_t out_max);
size_t menc_frame_bytes(const media_enc *m);
int    menc_frame_samples(const media_enc *m);    /* samples per channel per frame */
int    menc_bitpool(const media_enc *m);          /* 0 for aptX */
int    menc_set_bitpool(media_enc *m, int bitpool);

#endif
