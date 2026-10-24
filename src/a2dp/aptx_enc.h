/* aptX (classic) encoder for the A2DP source, 48 kHz stereo.
 * Port of FFmpeg's libavcodec/aptx.c + aptxenc.c, Copyright (C) 2017
 * Aurelien Jacobs <aurel@gnuage.org>, LGPL-2.1-or-later, used here under
 * GPL-3.0-or-later. aptX HD is not included.
 *
 * Every 4 stereo samples become two 16-bit codewords (4 bytes): 352 kbit/s
 * at 44.1 kHz, 384 kbit/s at 48 kHz. Same calling convention as
 * sbc_encoder_* (sbc.h) so the packer treats it like SBC with a fixed
 * "frame" of HB_APTX_FRAME_SAMPLES samples. */
#ifndef HEARBRIDGE_APTX_ENC_H
#define HEARBRIDGE_APTX_ENC_H

#include <stddef.h>
#include <stdint.h>

#define HB_APTX_FRAME_SAMPLES 128                      /* per channel, multiple of 4 */
#define HB_APTX_FRAME_BYTES   (HB_APTX_FRAME_SAMPLES)  /* 4 bytes per 4 samples */

typedef struct aptx_encoder aptx_encoder;

aptx_encoder *aptx_encoder_open(void);
void          aptx_encoder_close(aptx_encoder *e);

/* Encodes interleaved s16 stereo PCM (frames = samples per channel). Whole
 * HB_APTX_FRAME_SAMPLES blocks only: the rest waits for the next call.
 * Returns bytes written to out (a multiple of HB_APTX_FRAME_BYTES), 0 when
 * more input is needed, -1 on error. */
int aptx_encoder_encode(aptx_encoder *e, const int16_t *pcm, int frames,
                        unsigned char *out, size_t out_max);

#endif
