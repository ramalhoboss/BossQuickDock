/* libSceAvcap2 audio capture (Remote Play–style mix) for HearBridge.
 *
 * OpenAudio ABI + 0x38 param layout.
 */
#ifndef HEARBRIDGE_AVCAP2_H
#define HEARBRIDGE_AVCAP2_H

#include <stddef.h>
#include <stdint.h>

/* Streaming session for A2DP encode path (48 kHz stereo). */
typedef struct avcap2_session avcap2_session;

avcap2_session *avcap2_session_open(void);
void            avcap2_session_close(avcap2_session *s);

/* Diagnostics only: load libSceIpmi/libSceAvcap2 and check that the Avcap2
 * functions resolve (no capture is started). Results go to diag.h.
 * 0 = everything present. */
int avcap2_probe(void);

/* Snapshot taken by the capture owner, then published under the ctl lock.
 * State: 0 closed, 1 active, 2 awaiting PCM, 3 recovering, 4 failed, 5 transient. */
typedef struct {
    int state, pending_frames;
    long age_ms;
    uint32_t last_error;
    uint64_t valid, empty, overrun, recoveries, no_data, invalid, other, nonfinite;
} avcap2_stats;
void avcap2_session_stats(const avcap2_session *s, avcap2_stats *out);

/* Read up to max_frames stereo frames into interleaved s16le.
 * Returns frames read (>0), 0 if empty/transient or recovering,
 * -1 after bounded recovery is exhausted. Excess frames are kept for
 * subsequent calls before another library read. The library no-wait flag
 * stays enabled. Missing PCM for 5 s requests recovery; valid zero-valued
 * samples count as progress. Recovery budget refreshes after 30 s healthy.
 * Native IPC/locks remain synchronous; no universal call-time bound.
 * If peak_abs != NULL, writes max |sample| in [0,1] for this chunk (0 if empty). */
int avcap2_session_read_s16(avcap2_session *s, int16_t *out, int max_frames,
                            float *peak_abs);

int avcap2_session_read_latest_s16(avcap2_session *s,int16_t *out,int max_frames,float *peak,unsigned *skipped);
#endif
