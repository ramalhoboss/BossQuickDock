/* aptx_enc.c - aptX (classic) encoder, see aptx_enc.h.
 *
 * Port of FFmpeg's libavcodec/aptx.c and aptxenc.c:
 *   Copyright (C) 2017  Aurelien Jacobs <aurel@gnuage.org>
 *   FFmpeg is free software; you can redistribute it and/or modify it under
 *   the terms of the GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License, or (at your
 *   option) any later version.
 * Changes for BossQuickDock: aptX only (no HD), standalone C (FFmpeg's
 * helper macros written out below), s16 interleaved input and a fixed
 * 128-sample block per call. Distributed as part of BossQuickDock under
 * GPL-3.0-or-later (LGPL-2.1 section 3). */
#include "aptx_enc.h"

#include <stdlib.h>
#include <string.h>

#define MUL64(a, b) ((int64_t)(a) * (int64_t)(b))
#define FFABS(a)    ((a) >= 0 ? (a) : -(a))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFDIFFSIGN(x, y) (((x) > (y)) - ((x) < (y)))

enum { LEFT, RIGHT, NB_CHANNELS };
enum { LF, MLF, MHF, HF, NB_SUBBANDS };

#define NB_FILTERS 2
#define FILTER_TAPS 16

typedef struct {
    int pos;
    int32_t buffer[2 * FILTER_TAPS];
} FilterSignal;

typedef struct {
    FilterSignal outer_filter_signal[NB_FILTERS];
    FilterSignal inner_filter_signal[NB_FILTERS][NB_FILTERS];
} QMFAnalysis;

typedef struct {
    int32_t quantized_sample;
    int32_t quantized_sample_parity_change;
    int32_t error;
} Quantize;

typedef struct {
    int32_t quantization_factor;
    int32_t factor_select;
    int32_t reconstructed_difference;
} InvertQuantize;

typedef struct {
    int32_t prev_sign[2];
    int32_t s_weight[2];
    int32_t d_weight[24];
    int32_t pos;
    int32_t reconstructed_differences[48];
    int32_t previous_reconstructed_sample;
    int32_t predicted_difference;
    int32_t predicted_sample;
} Prediction;

typedef struct {
    int32_t codeword_history;
    int32_t dither_parity;
    int32_t dither[NB_SUBBANDS];
    QMFAnalysis qmf;
    Quantize quantize[NB_SUBBANDS];
    InvertQuantize invert_quantize[NB_SUBBANDS];
    Prediction prediction[NB_SUBBANDS];
} Channel;

typedef struct {
    const int32_t *quantize_intervals;
    const int32_t *invert_quantize_dither_factors;
    const int32_t *quantize_dither_factors;
    const int16_t *quantize_factor_select_offset;
    int tables_size;
    int32_t factor_max;
    int32_t prediction_order;
} ConstTables;

struct aptx_encoder {
    int32_t sync_idx;
    Channel channels[NB_CHANNELS];
    int16_t pend[2 * HB_APTX_FRAME_SAMPLES * 2];   /* up to two blocks waiting */
    int npend;                                      /* samples per channel in pend */
};

/* FFmpeg's av_clip_intp2() takes an int: wider values are truncated first. */
static inline int32_t clip_intp2(int32_t a, int p)
{
    int32_t lo = -((int32_t)1 << p), hi = ((int32_t)1 << p) - 1;
    return a < lo ? lo : a > hi ? hi : a;
}

static inline int32_t clip(int32_t a, int32_t lo, int32_t hi)
{
    return a < lo ? lo : a > hi ? hi : a;
}

/* Rounded right shift (round half to even), with optional 24-bit clip. */
static inline int32_t rshift32(int32_t value, int shift)
{
    int32_t rounding = (int32_t)1 << (shift - 1);
    int32_t mask = ((int32_t)1 << (shift + 1)) - 1;
    return ((value + rounding) >> shift) - ((value & mask) == rounding);
}

static inline int64_t rshift64(int64_t value, int shift)
{
    int64_t rounding = (int64_t)1 << (shift - 1);
    int64_t mask = ((int64_t)1 << (shift + 1)) - 1;
    return ((value + rounding) >> shift) - ((value & mask) == rounding);
}

static inline int32_t rshift32_clip24(int32_t value, int shift)
{
    return clip_intp2(rshift32(value, shift), 23);
}

static inline int32_t rshift64_clip24(int64_t value, int shift)
{
    return clip_intp2((int32_t)rshift64(value, shift), 23);
}

/* Convolution filter coefficients for the outer and inner QMF of the QMF
 * tree. The 2 sets of each are a mirror of each other. */
static const int32_t aptx_qmf_outer_coeffs[NB_FILTERS][FILTER_TAPS] = {
    {
        730, -413, -9611, 43626, -121026, 269973, -585547, 2801966,
        697128, -160481, 27611, 8478, -10043, 3511, 688, -897,
    },
    {
        -897, 688, 3511, -10043, 8478, 27611, -160481, 697128,
        2801966, -585547, 269973, -121026, 43626, -9611, -413, 730,
    },
};

static const int32_t aptx_qmf_inner_coeffs[NB_FILTERS][FILTER_TAPS] = {
    {
       1033, -584, -13592, 61697, -171156, 381799, -828088, 3962579,
       985888, -226954, 39048, 11990, -14203, 4966, 973, -1268,
    },
    {
      -1268, 973, 4966, -14203, 11990, 39048, -226954, 985888,
      3962579, -828088, 381799, -171156, 61697, -13592, -584, 1033,
    },
};

static const int32_t quantize_intervals_LF[65] = {
      -9948,    9948,   29860,   49808,   69822,   89926,  110144,  130502,
     151026,  171738,  192666,  213832,  235264,  256982,  279014,  301384,
     324118,  347244,  370790,  394782,  419250,  444226,  469742,  495832,
     522536,  549890,  577936,  606720,  636290,  666700,  698006,  730270,
     763562,  797958,  833538,  870398,  908640,  948376,  989740, 1032874,
    1077948, 1125150, 1174700, 1226850, 1281900, 1340196, 1402156, 1468282,
    1539182, 1615610, 1698514, 1789098, 1888944, 2000168, 2125700, 2269750,
    2438670, 2642660, 2899462, 3243240, 3746078, 4535138, 5664098, 7102424,
    8897462,
};
static const int32_t invert_quantize_dither_factors_LF[65] = {
       9948,   9948,   9962,   9988,  10026,  10078,  10142,  10218,
      10306,  10408,  10520,  10646,  10784,  10934,  11098,  11274,
      11462,  11664,  11880,  12112,  12358,  12618,  12898,  13194,
      13510,  13844,  14202,  14582,  14988,  15422,  15884,  16380,
      16912,  17484,  18098,  18762,  19480,  20258,  21106,  22030,
      23044,  24158,  25390,  26760,  28290,  30008,  31954,  34172,
      36728,  39700,  43202,  47382,  52462,  58762,  66770,  77280,
      91642, 112348, 144452, 199326, 303512, 485546, 643414, 794914,
    1000124,
};
static const int32_t quantize_dither_factors_LF[65] = {
        0,     4,     7,    10,    13,    16,    19,    22,
       26,    28,    32,    35,    38,    41,    44,    47,
       51,    54,    58,    62,    65,    70,    74,    79,
       84,    90,    95,   102,   109,   116,   124,   133,
      143,   154,   166,   180,   195,   212,   231,   254,
      279,   308,   343,   383,   430,   487,   555,   639,
      743,   876,  1045,  1270,  1575,  2002,  2628,  3591,
     5177,  8026, 13719, 26047, 45509, 39467, 37875, 51303,
        0,
};
static const int16_t quantize_factor_select_offset_LF[65] = {
      0, -21, -19, -17, -15, -12, -10,  -8,
     -6,  -4,  -1,   1,   3,   6,   8,  10,
     13,  15,  18,  20,  23,  26,  29,  31,
     34,  37,  40,  43,  47,  50,  53,  57,
     60,  64,  68,  72,  76,  80,  85,  89,
     94,  99, 105, 110, 116, 123, 129, 136,
    144, 152, 161, 171, 182, 194, 207, 223,
    241, 263, 291, 328, 382, 467, 522, 522,
    522,
};

static const int32_t quantize_intervals_MLF[9] = {
    -89806, 89806, 278502, 494338, 759442, 1113112, 1652322, 2720256, 5190186,
};
static const int32_t invert_quantize_dither_factors_MLF[9] = {
    89806, 89806, 98890, 116946, 148158, 205512, 333698, 734236, 1735696,
};
static const int32_t quantize_dither_factors_MLF[9] = {
    0, 2271, 4514, 7803, 14339, 32047, 100135, 250365, 0,
};
static const int16_t quantize_factor_select_offset_MLF[9] = {
    0, -14, 6, 29, 58, 96, 154, 270, 521,
};

static const int32_t quantize_intervals_MHF[3] = {
    -194080, 194080, 890562,
};
static const int32_t invert_quantize_dither_factors_MHF[3] = {
    194080, 194080, 502402,
};
static const int32_t quantize_dither_factors_MHF[3] = {
    0, 77081, 0,
};
static const int16_t quantize_factor_select_offset_MHF[3] = {
    0, -33, 136,
};

static const int32_t quantize_intervals_HF[5] = {
    -163006, 163006, 542708, 1120554, 2669238,
};
static const int32_t invert_quantize_dither_factors_HF[5] = {
    163006, 163006, 216698, 361148, 1187538,
};
static const int32_t quantize_dither_factors_HF[5] = {
    0, 13423, 36113, 206598, 0,
};
static const int16_t quantize_factor_select_offset_HF[5] = {
    0, -8, 33, 95, 262,
};

static const ConstTables aptx_quant_tables[NB_SUBBANDS] = {
    [LF]  = { quantize_intervals_LF, invert_quantize_dither_factors_LF,
              quantize_dither_factors_LF, quantize_factor_select_offset_LF,
              65, 0x11FF, 24 },
    [MLF] = { quantize_intervals_MLF, invert_quantize_dither_factors_MLF,
              quantize_dither_factors_MLF, quantize_factor_select_offset_MLF,
              9, 0x14FF, 12 },
    [MHF] = { quantize_intervals_MHF, invert_quantize_dither_factors_MHF,
              quantize_dither_factors_MHF, quantize_factor_select_offset_MHF,
              3, 0x16FF, 6 },
    [HF]  = { quantize_intervals_HF, invert_quantize_dither_factors_HF,
              quantize_dither_factors_HF, quantize_factor_select_offset_HF,
              5, 0x15FF, 12 },
};

static const int16_t quantization_factors[32] = {
    2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
    2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
    2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
    3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008,
};

/* ---- shared with the decoder in FFmpeg (aptx.c) ---- */

static void aptx_update_codeword_history(Channel *channel)
{
    int32_t cw = ((channel->quantize[0].quantized_sample & 3) << 0) +
                 ((channel->quantize[1].quantized_sample & 2) << 1) +
                 ((channel->quantize[2].quantized_sample & 1) << 3);
    channel->codeword_history = (cw << 8) + (int32_t)((unsigned)channel->codeword_history << 4);
}

static void aptx_generate_dither(Channel *channel)
{
    int subband;
    int64_t m;
    int32_t d;

    aptx_update_codeword_history(channel);

    m = (int64_t)5184443 * (channel->codeword_history >> 7);
    d = (int32_t)((m * 4) + (m >> 22));
    for (subband = 0; subband < NB_SUBBANDS; subband++)
        channel->dither[subband] = (int32_t)((unsigned)d << (23 - 5 * subband));
    channel->dither_parity = (d >> 25) & 1;
}

static void aptx_invert_quantization(InvertQuantize *invert_quantize,
                                     int32_t quantized_sample, int32_t dither,
                                     const ConstTables *tables)
{
    int32_t qr, idx, shift, factor_select;

    idx = (quantized_sample ^ -(quantized_sample < 0)) + 1;
    qr = tables->quantize_intervals[idx] / 2;
    if (quantized_sample < 0)
        qr = -qr;

    qr = rshift64_clip24(((int64_t)qr * ((int64_t)1 << 32)) +
                         MUL64(dither, tables->invert_quantize_dither_factors[idx]), 32);
    invert_quantize->reconstructed_difference =
        (int32_t)(MUL64(invert_quantize->quantization_factor, qr) >> 19);

    /* update factor_select */
    factor_select = 32620 * invert_quantize->factor_select;
    factor_select = rshift32(factor_select + (tables->quantize_factor_select_offset[idx] * (1 << 15)), 15);
    invert_quantize->factor_select = clip(factor_select, 0, tables->factor_max);

    /* update quantization factor */
    idx = (invert_quantize->factor_select & 0xFF) >> 3;
    shift = (tables->factor_max - invert_quantize->factor_select) >> 8;
    invert_quantize->quantization_factor = (quantization_factors[idx] << 11) >> shift;
}

static int32_t *aptx_reconstructed_differences_update(Prediction *prediction,
                                                      int32_t reconstructed_difference,
                                                      int order)
{
    int32_t *rd1 = prediction->reconstructed_differences, *rd2 = rd1 + order;
    int p = prediction->pos;

    rd1[p] = rd2[p];
    prediction->pos = p = (p + 1) % order;
    rd2[p] = reconstructed_difference;
    return &rd2[p];
}

static void aptx_prediction_filtering(Prediction *prediction,
                                      int32_t reconstructed_difference,
                                      int order)
{
    int32_t reconstructed_sample, predictor, srd0;
    int32_t *reconstructed_differences;
    int64_t predicted_difference = 0;
    int i;

    reconstructed_sample = clip_intp2(reconstructed_difference + prediction->predicted_sample, 23);
    predictor = clip_intp2((int32_t)((MUL64(prediction->s_weight[0], prediction->previous_reconstructed_sample)
                                      + MUL64(prediction->s_weight[1], reconstructed_sample)) >> 22), 23);
    prediction->previous_reconstructed_sample = reconstructed_sample;

    reconstructed_differences = aptx_reconstructed_differences_update(prediction, reconstructed_difference, order);
    srd0 = FFDIFFSIGN(reconstructed_difference, 0) * (1 << 23);
    for (i = 0; i < order; i++) {
        int32_t srd = (reconstructed_differences[-i - 1] >> 31) | 1;
        prediction->d_weight[i] -= rshift32(prediction->d_weight[i] - srd * srd0, 8);
        predicted_difference += MUL64(reconstructed_differences[-i], prediction->d_weight[i]);
    }

    prediction->predicted_difference = clip_intp2((int32_t)(predicted_difference >> 22), 23);
    prediction->predicted_sample = clip_intp2(predictor + prediction->predicted_difference, 23);
}

static void aptx_process_subband(InvertQuantize *invert_quantize,
                                 Prediction *prediction,
                                 int32_t quantized_sample, int32_t dither,
                                 const ConstTables *tables)
{
    int32_t sign, same_sign[2], weight[2], sw1, range;

    aptx_invert_quantization(invert_quantize, quantized_sample, dither, tables);

    sign = FFDIFFSIGN(invert_quantize->reconstructed_difference,
                      -prediction->predicted_difference);
    same_sign[0] = sign * prediction->prev_sign[0];
    same_sign[1] = sign * prediction->prev_sign[1];
    prediction->prev_sign[0] = prediction->prev_sign[1];
    prediction->prev_sign[1] = sign | 1;

    range = 0x100000;
    sw1 = rshift32(-same_sign[1] * prediction->s_weight[1], 1);
    sw1 = (clip(sw1, -range, range) & ~0xF) * 16;

    range = 0x300000;
    weight[0] = 254 * prediction->s_weight[0] + 0x800000 * same_sign[0] + sw1;
    prediction->s_weight[0] = clip(rshift32(weight[0], 8), -range, range);

    range = 0x3C0000 - prediction->s_weight[0];
    weight[1] = 255 * prediction->s_weight[1] + 0xC00000 * same_sign[1];
    prediction->s_weight[1] = clip(rshift32(weight[1], 8), -range, range);

    aptx_prediction_filtering(prediction,
                              invert_quantize->reconstructed_difference,
                              tables->prediction_order);
}

static void aptx_invert_quantize_and_prediction(Channel *channel)
{
    int subband;
    for (subband = 0; subband < NB_SUBBANDS; subband++)
        aptx_process_subband(&channel->invert_quantize[subband],
                             &channel->prediction[subband],
                             channel->quantize[subband].quantized_sample,
                             channel->dither[subband],
                             &aptx_quant_tables[subband]);
}

static inline int32_t aptx_quantized_parity(const Channel *channel)
{
    int32_t parity = channel->dither_parity;
    int subband;

    for (subband = 0; subband < NB_SUBBANDS; subband++)
        parity ^= channel->quantize[subband].quantized_sample;

    return parity & 1;
}

/* For each sample, ensure that the parity of all subbands of all channels
 * is 0 except once every 8 samples where the parity is forced to 1. */
static inline int aptx_check_parity(const Channel channels[NB_CHANNELS], int32_t *idx)
{
    int32_t parity = aptx_quantized_parity(&channels[LEFT])
                   ^ aptx_quantized_parity(&channels[RIGHT]);

    int eighth = *idx == 7;
    *idx = (*idx + 1) & 7;

    return parity ^ eighth;
}

/* ---- encoder (aptxenc.c) ---- */

static inline void aptx_qmf_filter_signal_push(FilterSignal *signal, int32_t sample)
{
    signal->buffer[signal->pos] = sample;
    signal->buffer[signal->pos + FILTER_TAPS] = sample;
    signal->pos = (signal->pos + 1) & (FILTER_TAPS - 1);
}

static inline int32_t aptx_qmf_convolution(const FilterSignal *signal,
                                           const int32_t coeffs[FILTER_TAPS],
                                           int shift)
{
    const int32_t *sig = &signal->buffer[signal->pos];
    int64_t e = 0;
    int i;

    for (i = 0; i < FILTER_TAPS; i++)
        e += MUL64(sig[i], coeffs[i]);

    return rshift64_clip24(e, shift);
}

/* Half-band QMF analysis filter realized with a polyphase FIR filter:
 * each pair of samples in gives one sample out, split into 2 subbands. */
static void aptx_qmf_polyphase_analysis(FilterSignal signal[NB_FILTERS],
                                        const int32_t coeffs[NB_FILTERS][FILTER_TAPS],
                                        int shift,
                                        const int32_t samples[NB_FILTERS],
                                        int32_t *low_subband_output,
                                        int32_t *high_subband_output)
{
    int32_t subbands[NB_FILTERS];
    int i;

    for (i = 0; i < NB_FILTERS; i++) {
        aptx_qmf_filter_signal_push(&signal[i], samples[NB_FILTERS - 1 - i]);
        subbands[i] = aptx_qmf_convolution(&signal[i], coeffs[i], shift);
    }

    *low_subband_output  = clip_intp2(subbands[0] + subbands[1], 23);
    *high_subband_output = clip_intp2(subbands[0] - subbands[1], 23);
}

/* Two stage QMF analysis tree: 4 input samples -> 1 sample in 4 subbands. */
static void aptx_qmf_tree_analysis(QMFAnalysis *qmf,
                                   const int32_t samples[4],
                                   int32_t subband_samples[4])
{
    int32_t intermediate_samples[4];
    int i;

    for (i = 0; i < 2; i++)
        aptx_qmf_polyphase_analysis(qmf->outer_filter_signal,
                                    aptx_qmf_outer_coeffs, 23,
                                    &samples[2 * i],
                                    &intermediate_samples[0 + i],
                                    &intermediate_samples[2 + i]);

    for (i = 0; i < 2; i++)
        aptx_qmf_polyphase_analysis(qmf->inner_filter_signal[i],
                                    aptx_qmf_inner_coeffs, 23,
                                    &intermediate_samples[2 * i],
                                    &subband_samples[2 * i + 0],
                                    &subband_samples[2 * i + 1]);
}

static inline int32_t aptx_bin_search(int32_t value, int32_t factor,
                                      const int32_t *intervals, int32_t nb_intervals)
{
    int32_t idx = 0;
    int i;

    for (i = nb_intervals >> 1; i > 0; i >>= 1)
        if (MUL64(factor, intervals[idx + i]) <= ((int64_t)value << 24))
            idx += i;

    return idx;
}

static void aptx_quantize_difference(Quantize *quantize,
                                     int32_t sample_difference,
                                     int32_t dither,
                                     int32_t quantization_factor,
                                     const ConstTables *tables)
{
    const int32_t *intervals = tables->quantize_intervals;
    int32_t quantized_sample, dithered_sample, parity_change;
    int32_t d, mean, interval, inv, sample_difference_abs;
    int64_t error;

    sample_difference_abs = FFABS(sample_difference);
    sample_difference_abs = FFMIN(sample_difference_abs, (1 << 23) - 1);

    quantized_sample = aptx_bin_search(sample_difference_abs >> 4,
                                       quantization_factor,
                                       intervals, tables->tables_size);

    d = rshift32_clip24((int32_t)(MUL64(dither, dither) >> 32), 7) - (1 << 23);
    d = (int32_t)rshift64(MUL64(d, tables->quantize_dither_factors[quantized_sample]), 23);

    intervals += quantized_sample;
    mean = (intervals[1] + intervals[0]) / 2;
    interval = (intervals[1] - intervals[0]) * (-(sample_difference < 0) | 1);

    dithered_sample = rshift64_clip24(MUL64(dither, interval) +
                                      ((int64_t)clip_intp2(mean + d, 23) << 32), 32);
    error = ((int64_t)sample_difference_abs << 20) - MUL64(dithered_sample, quantization_factor);
    quantize->error = (int32_t)FFABS(rshift64(error, 23));

    parity_change = quantized_sample;
    if (error < 0)
        quantized_sample--;
    else
        parity_change--;

    inv = -(sample_difference < 0);
    quantize->quantized_sample               = quantized_sample ^ inv;
    quantize->quantized_sample_parity_change = parity_change    ^ inv;
}

static void aptx_encode_channel(Channel *channel, const int32_t samples[4])
{
    int32_t subband_samples[4];
    int subband;
    aptx_qmf_tree_analysis(&channel->qmf, samples, subband_samples);
    aptx_generate_dither(channel);
    for (subband = 0; subband < NB_SUBBANDS; subband++) {
        int32_t diff = clip_intp2(subband_samples[subband] - channel->prediction[subband].predicted_sample, 23);
        aptx_quantize_difference(&channel->quantize[subband], diff,
                                 channel->dither[subband],
                                 channel->invert_quantize[subband].quantization_factor,
                                 &aptx_quant_tables[subband]);
    }
}

static void aptx_insert_sync(Channel channels[NB_CHANNELS], int32_t *idx)
{
    if (aptx_check_parity(channels, idx)) {
        int i;
        Channel *c;
        static const int map[] = { 1, 2, 0, 3 };
        Quantize *min = &channels[NB_CHANNELS - 1].quantize[map[0]];
        for (c = &channels[NB_CHANNELS - 1]; c >= channels; c--)
            for (i = 0; i < NB_SUBBANDS; i++)
                if (c->quantize[map[i]].error < min->error)
                    min = &c->quantize[map[i]];

        /* Forcing the desired parity is done by offsetting by 1 the quantized
         * sample from the subband featuring the smallest quantization error. */
        min->quantized_sample = min->quantized_sample_parity_change;
    }
}

static uint16_t aptx_pack_codeword(const Channel *channel)
{
    int32_t parity = aptx_quantized_parity(channel);
    return (uint16_t)((((channel->quantize[3].quantized_sample & 0x06) | parity) << 13)
                    | (((channel->quantize[2].quantized_sample & 0x03)         ) << 11)
                    | (((channel->quantize[1].quantized_sample & 0x0F)         ) <<  7)
                    | (((channel->quantize[0].quantized_sample & 0x7F)         ) <<  0));
}

/* 4 stereo samples (24-bit) -> 4 bytes: left then right codeword, big endian. */
static void aptx_encode_samples(aptx_encoder *e, int32_t samples[NB_CHANNELS][4], unsigned char *out)
{
    int channel;
    for (channel = 0; channel < NB_CHANNELS; channel++)
        aptx_encode_channel(&e->channels[channel], samples[channel]);

    aptx_insert_sync(e->channels, &e->sync_idx);

    for (channel = 0; channel < NB_CHANNELS; channel++) {
        uint16_t cw;
        aptx_invert_quantize_and_prediction(&e->channels[channel]);
        cw = aptx_pack_codeword(&e->channels[channel]);
        out[2 * channel]     = (unsigned char)(cw >> 8);
        out[2 * channel + 1] = (unsigned char)cw;
    }
}

aptx_encoder *aptx_encoder_open(void)
{
    aptx_encoder *e = calloc(1, sizeof *e);
    int chan, subband;
    if (!e) return NULL;
    for (chan = 0; chan < NB_CHANNELS; chan++)
        for (subband = 0; subband < NB_SUBBANDS; subband++) {
            e->channels[chan].prediction[subband].prev_sign[0] = 1;
            e->channels[chan].prediction[subband].prev_sign[1] = 1;
        }
    return e;
}

void aptx_encoder_close(aptx_encoder *e)
{
    free(e);
}

int aptx_encoder_encode(aptx_encoder *e, const int16_t *pcm, int frames,
                        unsigned char *out, size_t out_max)
{
    const int cap = 2 * HB_APTX_FRAME_SAMPLES;
    int wrote = 0;

    if (!e || !pcm || frames < 0 || !out) return -1;
    for (;;) {
        int take = cap - e->npend, pos;
        if (take > frames) take = frames;
        if (take > 0) {
            memcpy(e->pend + e->npend * 2, pcm, (size_t)take * 2 * sizeof *pcm);
            e->npend += take;
            pcm += take * 2;
            frames -= take;
        }
        if (e->npend < HB_APTX_FRAME_SAMPLES) break;             /* all input taken */
        if ((size_t)wrote + HB_APTX_FRAME_BYTES > out_max) break;  /* no room: rest dropped */
        for (pos = 0; pos < HB_APTX_FRAME_SAMPLES; pos += 4) {
            int32_t s[NB_CHANNELS][4];
            int k;
            for (k = 0; k < 4; k++) {
                s[LEFT][k]  = (int32_t)e->pend[(pos + k) * 2]     * 256;   /* s16 -> 24 bit */
                s[RIGHT][k] = (int32_t)e->pend[(pos + k) * 2 + 1] * 256;
            }
            aptx_encode_samples(e, s, out + wrote + pos);
        }
        wrote += HB_APTX_FRAME_BYTES;
        e->npend -= HB_APTX_FRAME_SAMPLES;
        memmove(e->pend, e->pend + HB_APTX_FRAME_SAMPLES * 2, (size_t)e->npend * 2 * sizeof *e->pend);
    }
    return wrote;
}
