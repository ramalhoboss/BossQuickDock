/* A2DP media packet builder + debug dump. No link dependency, so the host
 * tests build the exact bytes the console sends. Developed by X-F1REBALL-X. */
#include "avdtp.h"
#include "hsprefs.h"

#include <stdio.h>
#include <string.h>

int avdtp_sbc_pick_mode(unsigned char caps0, int *joint, const char **why)
{
    unsigned char out = 0x10;                     /* 48 kHz */
    *joint = 0;
    *why = NULL;
    /* freq: bit7=16k bit6=32k bit5=44.1 bit4=48 */
    if (!(caps0 & 0x10)) {
        *why = (caps0 & 0x20) ? "the headset takes 44.1 kHz but not 48 kHz (no resampler)"
                              : "the headset takes neither 44.1 nor 48 kHz";
        return 0;
    }
    /* channel: bit3=mono bit2=dual bit1=stereo bit0=joint */
    if (caps0 & 0x01) { out |= 0x01; *joint = 1; }
    else if (caps0 & 0x02) out |= 0x02;
    else if (caps0 & 0x04) out |= 0x04;
    else {
        *why = (caps0 & 0x08) ? "the headset takes mono only (no downmix)"
                              : "the headset takes no stereo mode";
        return 0;
    }
    return out;
}

int avdtp_sbc_pick(const uint8_t caps[4], int want, int no_xq, avdtp_codec_pick *o,
                   const char **why, int held_codec, int held_bp)
{
    int joint = 0, lo = caps[2], hi = caps[3];
    unsigned char c1 = caps[1], out0, out1 = 0;

    memset(o, 0, sizeof *o);
    out0 = (unsigned char)avdtp_sbc_pick_mode(caps[0], &joint, why);
    if (!out0) return 0;
    if (lo < 2) lo = 2;
    if (hi < lo) hi = lo;

    /* A2DP 4.3.2: block length bit7=4 bit6=8 bit5=12 bit4=16; subbands
     * bit3=4 bit2=8; allocation bit1=SNR bit0=Loudness. Prefer 16 blocks,
     * 8 subbands, Loudness: 128 samples/frame, 375 frames/s at 48 kHz. */
    if (c1 & 0x10) out1 |= 0x10;
    else if (c1 & 0x20) out1 |= 0x20;
    else if (c1 & 0x40) out1 |= 0x40;
    else out1 |= 0x80;
    out1 |= (c1 & 0x04) ? 0x04 : 0x08;
    out1 |= (c1 & 0x01) ? 0x01 : 0x02;

    /* What the sink's advertised parameters really allow. HQ and XQ are
     * defined for 16 blocks + 8 subbands (128 samples/frame):
     *   SBC HQ: joint/stereo with a bitpool above 53 (to the sink max);
     *   SBC-XQ: dual channel, bitpool 35..38 per channel.
     * 48 kHz is already required above. */
    o->avail = 1 << HB_CODEC_SBC;
    if ((c1 & 0x10) && (c1 & 0x04)) {
        if (hi > 53 && (out0 & 0x03)) o->avail |= 1 << HB_CODEC_SBC_HQ;
        if ((caps[0] & 0x04) && hi >= HB_XQ_MIN_BP) o->avail |= 1 << HB_CODEC_SBC_XQ;
    }

    /* Auto is plain SBC on every headset. HQ and XQ only when the page
     * asks for them, and only if the sink advertised them. A remembered
     * XQ is not where auto starts. */
    if (want == HB_CODEC_AUTO)
        want = HB_CODEC_SBC;
    else if (want < HB_CODEC_SBC || want >= HB_CODEC_N || !(o->avail & (1 << want)))
        want = HB_CODEC_SBC;
    (void)no_xq;

    o->codec = want;
    o->cfg[1] = out1;
    o->cfg[2] = (unsigned char)lo;
    if (want == HB_CODEC_SBC_XQ) {
        o->cfg[0] = 0x10 | 0x04;                  /* 48 kHz, dual channel */
        o->ceil = hi < HB_XQ_CEIL ? hi : HB_XQ_CEIL;
        o->name = "SBC-XQ";
    } else if (want == HB_CODEC_SBC_HQ) {
        o->cfg[0] = out0;
        o->ceil = hi < HB_HQ_MAX_BP ? hi : HB_HQ_MAX_BP;
        o->name = "SBC HQ";
    } else {
        o->cfg[0] = out0;
        o->ceil = hi < 53 ? hi : 53;
        o->name = "SBC";
    }
    if (o->ceil < lo) o->ceil = lo;
    o->cfg[3] = (unsigned char)o->ceil;
    o->start_bp = 35 < o->ceil ? 35 : o->ceil;
    /* Only the bitpool that held on this same codec. An XQ number must
     * not become the SBC start. */
    if (held_codec == want && held_bp >= lo && held_bp <= o->ceil) o->start_bp = held_bp;
    if (o->start_bp < lo) o->start_bp = lo;
    return 1;
}

int avdtp_aptx_caps_ok(const unsigned char *caps, int len, int *delay)
{
    int i = 0, found = 0;
    if (delay) *delay = 0;
    while (caps && i + 1 < len) {
        int cat = caps[i], clen = caps[i + 1];
        const unsigned char *b = caps + i + 2;
        if (i + 2 + clen > len) break;
        /* Media Codec: Audio, vendor specific, Qualcomm aptX, 48 kHz + stereo */
        if (cat == 0x07 && clen >= AVDTP_APTX_IE_LEN && (b[0] >> 4) == 0 && b[1] == 0xFF &&
            b[2] == 0x4F && b[3] == 0 && b[4] == 0 && b[5] == 0 && b[6] == 0x01 && b[7] == 0x00 &&
            (b[8] & 0x10) && (b[8] & 0x02))
            found = 1;
        else if (cat == 0x08 && delay)
            *delay = 1;
        i += 2 + clen;
    }
    return found;
}

void avdtp_aptx_ie(unsigned char ie[AVDTP_APTX_IE_LEN])
{
    static const unsigned char v[AVDTP_APTX_IE_LEN] = {
        0x00, 0xFF, 0x4F, 0x00, 0x00, 0x00, 0x01, 0x00, AVDTP_APTX_48K_STEREO };
    memcpy(ie, v, sizeof v);
}

int avdtp_build_media(avdtp_session *s, const unsigned char *sbc_frames, int len,
                      int samples_in_packet, int n_sbc_frames,
                      unsigned char *pkt, int pkt_max)
{
    int nf = n_sbc_frames > 0 ? n_sbc_frames : 1;
    if (s && s->codec.codec == HB_CODEC_APTX) {
        /* aptX: the codewords alone, no RTP or media payload header. */
        if (len <= 0 || len > pkt_max) return 0;
        memcpy(pkt, sbc_frames, (size_t)len);
        s->rtp_seq++;
        s->rtp_ts += (uint32_t)(samples_in_packet > 0 ? samples_in_packet : 128);
        return len;
    }
    if (!s || len <= 0 || 13 + len > pkt_max) return 0;
    if (nf > 15) nf = 15;              /* 4-bit NUM field, non-fragmented */
    /* RTP (RFC 3550): V=2 P=0 X=0 CC=0 | M=0 PT=96 | seq | ts | SSRC */
    pkt[0] = 0x80;
    pkt[1] = 0x60;
    pkt[2] = (unsigned char)(s->rtp_seq >> 8);
    pkt[3] = (unsigned char)s->rtp_seq;
    pkt[4] = (unsigned char)(s->rtp_ts >> 24);
    pkt[5] = (unsigned char)(s->rtp_ts >> 16);
    pkt[6] = (unsigned char)(s->rtp_ts >> 8);
    pkt[7] = (unsigned char)s->rtp_ts;
    pkt[8] = (unsigned char)(s->rtp_ssrc >> 24);
    pkt[9] = (unsigned char)(s->rtp_ssrc >> 16);
    pkt[10] = (unsigned char)(s->rtp_ssrc >> 8);
    pkt[11] = (unsigned char)s->rtp_ssrc;
    /* A2DP SBC media payload header: F=S=L=RFA=0, NUM = frame count */
    pkt[12] = (unsigned char)(nf & 0x0F);
    memcpy(pkt + 13, sbc_frames, (size_t)len);
    s->rtp_seq++;
    s->rtp_ts += (uint32_t)(samples_in_packet > 0 ? samples_in_packet : 128);
    return 13 + len;
}

/* Dump format: "HBMD1\n" magic, 4-byte SBC IE (as configured), then per
 * packet a little-endian u16 length + the L2CAP payload (RTP included). */
int avdtp_dump_open(avdtp_session *s, const char *path, int max_pkts)
{
    FILE *f;
    if (!s || !path) return 0;
    if (s->codec.codec == HB_CODEC_APTX) return 0;   /* the dump format and checker are SBC only */
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("HBMD1\n", 1, 6, f);
    fwrite(s->sbc_cfg, 1, 4, f);
    s->dump = f;
    s->dump_left = max_pkts;
    return 1;
}

void avdtp_dump_packet(avdtp_session *s, const unsigned char *p, int n)
{
    unsigned char h[2];
    FILE *f;
    if (!s || !s->dump || n <= 0) return;
    f = (FILE *)s->dump;
    h[0] = (unsigned char)n;
    h[1] = (unsigned char)(n >> 8);
    fwrite(h, 1, 2, f);
    fwrite(p, 1, (size_t)n, f);
    if (--s->dump_left <= 0) avdtp_dump_close(s);
}

void avdtp_dump_close(avdtp_session *s)
{
    if (s && s->dump) {
        fclose((FILE *)s->dump);
        s->dump = NULL;
    }
}
