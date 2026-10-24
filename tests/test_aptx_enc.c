/* Host test for the aptX encoder: encodes a stereo tone pair (48 kHz,
 * 3 s) in uneven chunks, the way the packer feeds it, and writes the raw
 * aptX stream + the reference PCM. `make test-aptx` then checks it byte for
 * byte against FFmpeg's encoder and decodes it with FFmpeg for the SNR.
 * Usage: test_aptx_enc out.aptx ref.s16 */
#include "aptx_enc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS   48000
#define SECS 3

int main(int argc, char **argv)
{
    const int n = FS * SECS;
    int16_t *pcm;
    unsigned char out[2 * HB_APTX_FRAME_BYTES];
    aptx_encoder *e;
    FILE *fo, *fr;
    int i, off = 0, bad = 0, chunk = 0;
    long bytes = 0;
    static const int sizes[] = { 37, 128, 1, 300, 64, 513, 128, 7 };

    if (argc < 3) return 2;
    pcm = malloc(sizeof *pcm * 2 * (size_t)n);
    if (!pcm) return 1;
    for (i = 0; i < n; i++) {
        double t = (double)i / FS;
        pcm[2 * i]     = (int16_t)lrint(12000.0 * sin(2 * M_PI * 1000.0 * t) + 3000.0 * sin(2 * M_PI * 6300.0 * t));
        pcm[2 * i + 1] = (int16_t)lrint(10000.0 * sin(2 * M_PI * 523.25 * t) + 2500.0 * sin(2 * M_PI * 11025.0 * t));
    }
    e = aptx_encoder_open();
    fo = fopen(argv[1], "wb");
    fr = fopen(argv[2], "wb");
    if (!e || !fo || !fr) return 1;
    fwrite(pcm, sizeof *pcm, 2 * (size_t)n, fr);

    /* At most HB_APTX_FRAME_SAMPLES per call, like the packer (chunk <= samples_per). */
    while (off < n) {
        int c = sizes[chunk++ % (int)(sizeof sizes / sizeof *sizes)], got;
        if (c > HB_APTX_FRAME_SAMPLES) c = HB_APTX_FRAME_SAMPLES;
        if (c > n - off) c = n - off;
        got = aptx_encoder_encode(e, pcm + 2 * off, c, out, sizeof out);
        if (got < 0 || got % HB_APTX_FRAME_BYTES) { fprintf(stderr, "FAIL encode returned %d\n", got); bad = 1; break; }
        fwrite(out, 1, (size_t)got, fo);
        bytes += got;
        off += c;
    }
    if (aptx_encoder_encode(e, pcm, 0, out, sizeof out) != 0) { fprintf(stderr, "FAIL empty call\n"); bad = 1; }
    if (aptx_encoder_encode(e, pcm, HB_APTX_FRAME_SAMPLES, out, HB_APTX_FRAME_BYTES - 1) != 0) {
        fprintf(stderr, "FAIL no room must write nothing\n");
        bad = 1;
    }
    if (aptx_encoder_encode(NULL, pcm, 4, out, sizeof out) != -1) { fprintf(stderr, "FAIL NULL encoder\n"); bad = 1; }
    /* 4 bytes per 4 stereo samples: 48000 B/s = 384 kbit/s. */
    if (bytes != (long)(n / HB_APTX_FRAME_SAMPLES) * HB_APTX_FRAME_BYTES) {
        fprintf(stderr, "FAIL %ld bytes for %d samples\n", bytes, n);
        bad = 1;
    }
    fclose(fo);
    fclose(fr);
    aptx_encoder_close(e);
    free(pcm);
    printf("%s   aptX encoder: %ld bytes (%d kbit/s at 48 kHz), chunked input\n",
           bad ? "FAIL" : "ok", bytes, (int)(bytes * 8 / SECS / 1000));
    return bad;
}
