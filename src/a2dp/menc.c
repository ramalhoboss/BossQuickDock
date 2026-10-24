/* menc.c - SBC or aptX for the packer, see menc.h. */
#include "menc.h"
#include "aptx_enc.h"

#include <stdlib.h>

struct media_enc {
    sbc_encoder *sbc;
    aptx_encoder *aptx;
};

media_enc *menc_open_sbc(const sbc_config *cfg)
{
    media_enc *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->sbc = sbc_encoder_open(cfg);
    if (!m->sbc) { free(m); return NULL; }
    return m;
}

media_enc *menc_open_aptx(void)
{
    media_enc *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->aptx = aptx_encoder_open();
    if (!m->aptx) { free(m); return NULL; }
    return m;
}

void menc_close(media_enc *m)
{
    if (!m) return;
    if (m->sbc) sbc_encoder_close(m->sbc);
    if (m->aptx) aptx_encoder_close(m->aptx);
    free(m);
}

int menc_is_aptx(const media_enc *m)
{
    return m && m->aptx;
}

int menc_encode(media_enc *m, const int16_t *pcm, int frames, unsigned char *out, size_t out_max)
{
    if (!m) return -1;
    if (m->aptx) return aptx_encoder_encode(m->aptx, pcm, frames, out, out_max);
    return sbc_encoder_encode(m->sbc, pcm, frames, out, out_max);
}

size_t menc_frame_bytes(const media_enc *m)
{
    if (!m) return 0;
    return m->aptx ? HB_APTX_FRAME_BYTES : sbc_encoder_frame_bytes(m->sbc);
}

int menc_frame_samples(const media_enc *m)
{
    if (!m) return 0;
    return m->aptx ? HB_APTX_FRAME_SAMPLES : sbc_encoder_frame_samples(m->sbc);
}

int menc_bitpool(const media_enc *m)
{
    if (!m || m->aptx) return 0;
    return sbc_encoder_bitpool(m->sbc);
}

int menc_set_bitpool(media_enc *m, int bitpool)
{
    if (!m || m->aptx) return 0;
    return sbc_encoder_set_bitpool(m->sbc, bitpool);
}
