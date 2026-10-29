/* HearBridge — system audio capture through libSceAvcap2 (48 kHz stereo).
 *
 * NIDs + OpenAudio ABI: 16-byte meta, buflen=0x10000, no authid elevation
 * by default. Reads use the no-wait flag; recovery is bounded and paced.
 */
#include "avcap2.h"
#include "capture_guard.h"
#include "creds.h"
#include "diag.h"
#include "log.h"
#include "util.h"

#include <ps5/kernel.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NID_LoadStartModule "wzvqT4UqKX8"

#define NID_Avcap2Initialize "svzPXluOz8U"
#define NID_Avcap2Terminate  "gojqghDU+1Y"
#define NID_Avcap2OpenAudio  "3nHpE7Dp5SE"
#define NID_Avcap2Close      "tR7gPe1i8hw"
#define NID_Avcap2Start      "8CGNCwBsItI"
#define NID_Avcap2Stop       "Z5dKK0xnQ8g"
#define NID_Avcap2ReadAudio  "WhsHggqdwcg"

/* Conditions verified in the captured ReadAudio path, not bit flags. */
#define AVCAP2_INVALID  0x81950001u
#define AVCAP2_EMPTY    0x81950002u
#define AVCAP2_UNMAPPED 0x81950003u
#define AVCAP2_OVERRUN  0x81950004u
/* No-wait bypasses the library's extra availability/wait loop. Locks and
 * other native calls may still block; this is not a universal time bound. */
#define AVCAP2_READ_NOWAIT 1

#define OPEN_PARAM_SIZE 0x38
#define META_SIZE       0x10
#define SAMPLE_RATE     48000
#define CHANNELS        2
/* ReadAudio hands over one whole packet per call. A packet bigger than
 * buflen gets 0x81950001 and the library does not move past it, so the
 * same error comes back on every read. Packets are 1024 frames (8 KB of
 * stereo float); 64 KB also holds 1024 frames of 12-channel float. */
#define READ_BYTES      0x10000
#define RECOVERY_LIMIT  3
#define RECOVERY_GAP_MS 1000
#define PROBLEM_GRACE_MS 1000
#define NO_DATA_TIMEOUT_MS 5000
#define HEALTHY_RESET_MS 30000

/* No authid elevation needed for Avcap2 capture. We try default first. */
#define AUTHID_AVCAP2   0x4900000000000002ULL
#define AUTHID_FALLBACK 0x4801000000000013ULL

typedef int (*fn_LoadStart)(const char *, size_t, const void *, uint32_t, void *, int *);
typedef int (*fn_Init)(void);
typedef int (*fn_Term)(void);
typedef int (*fn_Open)(int64_t *outhandle, void *param, int flags);
typedef int (*fn_Close)(int64_t handle);
typedef int (*fn_Start)(int64_t handle);
typedef int (*fn_Stop)(int64_t handle);
typedef int (*fn_Read)(int64_t handle, void *buf, size_t buflen, void *out_meta, int flag);

static int resolve_libkernel(uint32_t *out)
{
    static const char *names[] = {
        "libkernel_web.sprx", "libkernel.sprx", "libkernel_sys.sprx", NULL
    };
    int i;

    for (i = 0; names[i]; i++) {
        if (kernel_dynlib_handle(-1, names[i], out) == 0 && *out)
            return 0;
    }
    return -1;
}

static int load_sprx(fn_LoadStart LoadStart, const char *basename)
{
    static const char *roots[] = {
        "/system/common/lib/",
        "",
        "/system/priv/lib/",
        "/system_ex/common_ex/lib/",
        "/system_ex/priv_ex/lib/",
        NULL
    };
    uint32_t handle = 0;
    char path[256];
    int i, res, rv;

    if (kernel_dynlib_handle(-1, basename, &handle) == 0 && handle) {
        log_line("avcap2: %s already loaded (handle %#x)", basename, handle);
        diag_set(basename, "already loaded (handle %#x)", handle);
        return 0;
    }
    if (!LoadStart) {
        log_line("avcap2: no sceKernelLoadStartModule");
        diag_set(basename, "FAILED: sceKernelLoadStartModule not resolved");
        return -1;
    }
    for (i = 0; roots[i]; i++) {
        if (roots[i][0])
            snprintf(path, sizeof path, "%s%s", roots[i], basename);
        else
            snprintf(path, sizeof path, "%s", basename);
        res = -1;
        rv = LoadStart(path, 0, 0, 0, 0, &res);
        log_line("avcap2: LoadStart %s -> rv=%d res=%d", path, rv, res);
        if (kernel_dynlib_handle(-1, basename, &handle) == 0 && handle) {
            log_line("avcap2: %s handle %#x", basename, handle);
            diag_set(basename, "loaded from %s (rv %d, res %d, handle %#x)", path, rv, res, handle);
            return 0;
        }
    }
    log_line("avcap2: failed to load %s", basename);
    diag_set(basename, "FAILED to load (last try %s: rv %d res %d)", path, rv, res);
    return -1;
}

/* Same switch as before 1.0.1-fw13.60 (no restore): see creds.c. */
static void elevate(uint64_t authid)
{
    (void)creds_elevate(authid, NULL, "avcap2");
}

static void fill_open_param(unsigned char *p)
{
    memset(p, 0, OPEN_PARAM_SIZE);
    *(uint64_t *)(p + 0x00) = OPEN_PARAM_SIZE; /* 0x38 */
    *(uint64_t *)(p + 0x08) = 0x18;
    *(uint32_t *)(p + 0x14) = 2;               /* source = Remote Play mix */
    *(uint32_t *)(p + 0x1c) = 0xffffffffu;
}

/* OpenAudio param layout (16-byte meta). First u32 OUT is
 * typically 0x10 (size). Pre-set size in case the library treats it as IN/OUT. */
static void fill_meta(unsigned char *m)
{
    memset(m, 0, META_SIZE);
    *(uint32_t *)(m + 0x00) = META_SIZE;
}

static void hexdump_n(const char *tag, const unsigned char *p, int n)
{
    char line[128];
    int i, off = 0;

    while (off < n) {
        int pos = 0;
        for (i = 0; i < 16 && off + i < n; i++)
            pos += snprintf(line + pos, sizeof line - (size_t)pos, "%02x%s",
                            p[off + i], (i + 1) % 16 ? " " : "");
        line[pos] = '\0';
        log_line("avcap2: %s +%#x %s", tag, off, line);
        off += 16;
    }
}

/* Open+Start with OpenAudio param. auth_mode: 0=leave authid, 1=Avcap2, 2=fallback. */
static int open_and_start(fn_Init Init, fn_Term Term, fn_Open Open, fn_Close Close,
                          fn_Start Start, int64_t *outhandle, int auth_mode)
{
    unsigned char open_param[OPEN_PARAM_SIZE];
    int64_t handle = 0;
    int rv;

    if (auth_mode == 1)
        elevate(AUTHID_AVCAP2);
    else if (auth_mode == 2)
        elevate(AUTHID_FALLBACK);
    else
        log_line("avcap2: leaving authid %#llx (no elevate)",
                 (unsigned long long)kernel_get_ucred_authid(-1));

    log_line("avcap2: before Initialize (auth_mode=%d)", auth_mode);
    rv = Init();
    log_line("avcap2: Initialize -> %#x", (unsigned)rv);
    if (rv != 0) {
        if (Term) {
            log_line("avcap2: before Terminate (Init failed, clear)");
            log_line("avcap2: Terminate -> %#x", (unsigned)Term());
        }
        log_line("avcap2: before Initialize (retry)");
        rv = Init();
        log_line("avcap2: Initialize (retry) -> %#x", (unsigned)rv);
        if (rv != 0) {
            diag_set("audio open", "auth_mode %d: sceAvcap2Initialize failed %#x", auth_mode, (unsigned)rv);
            return rv;
        }
    }

    fill_open_param(open_param);
    log_line("avcap2: OpenAudio param layout (3-arg, handle*+param+0)");
    hexdump_n("param", open_param, OPEN_PARAM_SIZE);

    log_line("avcap2: before Open(&handle, param, 0)");
    rv = Open(&handle, open_param, 0);
    log_line("avcap2: Open -> rv=%#x handle=%#llx",
             (unsigned)rv, (unsigned long long)handle);
    diag_set("audio open", "auth_mode %d: Initialize ok, OpenAudio rv %#x handle %#llx",
             auth_mode, (unsigned)rv, (unsigned long long)handle);
    if (rv != 0 || handle == 0) {
        if (Term) {
            log_line("avcap2: before Terminate (Open failed)");
            log_line("avcap2: Terminate -> %#x", (unsigned)Term());
        }
        return rv != 0 ? rv : -1;
    }

    log_line("avcap2: before Start(handle=%#llx)", (unsigned long long)handle);
    rv = Start(handle);
    log_line("avcap2: Start -> %#x", (unsigned)rv);
    if (rv != 0) {
        diag_set("audio open", "auth_mode %d: Start failed %#x", auth_mode, (unsigned)rv);
        Close(handle);
        if (Term) {
            log_line("avcap2: before Terminate (Start failed)");
            log_line("avcap2: Terminate -> %#x", (unsigned)Term());
        }
        return rv;
    }

    *outhandle = handle;
    return 0;
}

/* capture_restart: Stop/Close/Term then start_capture again. */
static int session_restart(fn_Init Init, fn_Term Term, fn_Open Open, fn_Close Close,
                           fn_Start Start, fn_Stop Stop, int64_t *handle, int auth_mode)
{
    log_line("avcap2: session_restart (handle=%#llx auth_mode=%d)",
             (unsigned long long)*handle, auth_mode);
    if (*handle) {
        Stop(*handle);
        Close(*handle);
        *handle = 0;
    }
    if (Term) {
        log_line("avcap2: Terminate (restart) -> %#x", (unsigned)Term());
    }
    return open_and_start(Init, Term, Open, Close, Start, handle, auth_mode);
}

/* ---- streaming session (A2DP path) ---------------------------------------- */

struct avcap2_session {
    fn_Init Init;
    fn_Term Term;
    fn_Open Open;
    fn_Close Close;
    fn_Start Start;
    fn_Stop Stop;
    fn_Read Read;
    int64_t handle;
    int auth_mode;
    float *buf;
    long last_ok_ms;
    long watchdog_since_ms;
    int awaiting_pcm;
    int recovering;
    int failed;
    uint64_t reads_ok;
    uint64_t reads_empty;
    uint64_t reads_invalid;
    uint64_t reads_other;
    int logged_first;
    /* Detected layout of the capture buffer. Defaults: float32, 2 ch. */
    int fmt_s16;          /* 1 = int16 samples, 0 = float32 */
    int nch;              /* interleaved channels in the buffer */
    int fmt_checked;
    long rate_t0;         /* channel-count check: bytes over ~2 s */
    uint64_t rate_bytes;
    int rate_done;
    uint64_t nonfinite;
    /* A read can contain more frames than the caller can consume. Keep
     * the raw read and its detected layout until every frame is drained. */
    int pending_frames;
    int pending_offset;
    int pending_nch;
    int pending_s16;
    int recoveries;
    long recover_at_ms;
    long problem_since_ms;
    int problem_active;
    long healthy_since_ms;
    int healthy_active;
    long last_health_diag_ms;
    uint32_t last_error;
    const char *recovery_reason;
    uint64_t recoveries_total;
    uint64_t reads_unmapped, reads_overrun, reads_zero;
    uint64_t no_data_events;
};

void avcap2_session_stats(const avcap2_session *s, avcap2_stats *out)
{
    long now;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s) return;
    now = now_ms();
    out->state = s->failed ? 4 : s->recovering ? 3 : s->awaiting_pcm ? 2 : s->problem_active ? 5 : 1;
    out->age_ms = now >= s->last_ok_ms ? now - s->last_ok_ms : 0;
    out->pending_frames = s->pending_frames;
    out->last_error = s->last_error;
    out->valid = s->reads_ok; out->empty = s->reads_empty;
    out->overrun = s->reads_overrun; out->recoveries = s->recoveries_total;
    out->no_data = s->no_data_events; out->invalid = s->reads_invalid;
    out->other = s->reads_other; out->nonfinite = s->nonfinite;
}

int avcap2_probe(void)
{
    static const char *const nids[] = {
        NID_Avcap2Initialize, NID_Avcap2OpenAudio, NID_Avcap2Start,
        NID_Avcap2ReadAudio, NID_Avcap2Stop, NID_Avcap2Close,
    };
    uint32_t kh = 0, ah = 0;
    fn_LoadStart LoadStart;
    int i, missing = 0;

    if (resolve_libkernel(&kh) != 0) {
        diag_set("audio libs", "FAILED: libkernel handle not found");
        return -1;
    }
    LoadStart = (fn_LoadStart)kernel_dynlib_resolve(-1, kh, NID_LoadStartModule);
    if (load_sprx(LoadStart, "libSceIpmi.sprx") != 0 ||
        load_sprx(LoadStart, "libSceAvcap2.sprx") != 0) {
        diag_set("audio libs", "FAILED: see the libSce*.sprx lines");
        return -1;
    }
    if (kernel_dynlib_handle(-1, "libSceAvcap2.sprx", &ah) != 0 || !ah) {
        diag_set("audio libs", "FAILED: no libSceAvcap2 handle");
        return -1;
    }
    for (i = 0; i < (int)(sizeof nids / sizeof nids[0]); i++)
        if (!kernel_dynlib_resolve(-1, ah, nids[i])) missing++;
    if (missing) diag_set("audio libs", "loaded, %d Avcap2 function(s) missing", missing);
    else diag_set("audio libs", "loaded, all Avcap2 functions resolved");
    log_line("avcap2: probe: %d of %d functions missing", missing,
             (int)(sizeof nids / sizeof nids[0]));
    return missing ? -1 : 0;
}

static avcap2_session *session_open_impl(void)
{
    avcap2_session *s;
    uint32_t kh = 0, ah = 0;
    fn_LoadStart LoadStart = NULL;
    int mode, opened = 0;

    s = calloc(1, sizeof *s);
    if (!s) return NULL;

    if (resolve_libkernel(&kh) != 0) {
        log_line("avcap2: libkernel not found");
        free(s);
        return NULL;
    }
    LoadStart = (fn_LoadStart)kernel_dynlib_resolve(-1, kh, NID_LoadStartModule);
    if (load_sprx(LoadStart, "libSceIpmi.sprx") != 0 ||
        load_sprx(LoadStart, "libSceAvcap2.sprx") != 0) {
        free(s);
        return NULL;
    }
    if (kernel_dynlib_handle(-1, "libSceAvcap2.sprx", &ah) != 0 || !ah) {
        free(s);
        return NULL;
    }
    s->Init  = (fn_Init)kernel_dynlib_resolve(-1, ah, NID_Avcap2Initialize);
    s->Term  = (fn_Term)kernel_dynlib_resolve(-1, ah, NID_Avcap2Terminate);
    s->Open  = (fn_Open)kernel_dynlib_resolve(-1, ah, NID_Avcap2OpenAudio);
    s->Close = (fn_Close)kernel_dynlib_resolve(-1, ah, NID_Avcap2Close);
    s->Start = (fn_Start)kernel_dynlib_resolve(-1, ah, NID_Avcap2Start);
    s->Stop  = (fn_Stop)kernel_dynlib_resolve(-1, ah, NID_Avcap2Stop);
    s->Read  = (fn_Read)kernel_dynlib_resolve(-1, ah, NID_Avcap2ReadAudio);
    if (!s->Init || !s->Open || !s->Start || !s->Read || !s->Stop || !s->Close) {
        log_line("avcap2: missing NIDs");
        diag_set("audio session", "FAILED: Avcap2 functions missing");
        (void)diag_save();
        free(s);
        return NULL;
    }
    s->buf = (float *)malloc((size_t)READ_BYTES);
    if (!s->buf) { free(s); return NULL; }

    for (mode = 0; mode <= 2; mode++) {
        if (open_and_start(s->Init, s->Term, s->Open, s->Close, s->Start,
                           &s->handle, mode) == 0 && s->handle) {
            s->auth_mode = mode;
            opened = 1;
            break;
        }
        s->handle = 0;
    }
    if (!opened) {
        log_line("avcap2: session Open/Start failed");
        diag_set("audio session", "FAILED: Open/Start failed in every auth mode");
        (void)diag_save();
        free(s->buf);
        free(s);
        return NULL;
    }
    s->nch = CHANNELS;
    s->last_ok_ms = now_ms();
    s->watchdog_since_ms=s->last_ok_ms;
    s->awaiting_pcm=1;
    usleep(100000);
    log_line("avcap2: session open for streaming");
    diag_set("audio session", "ok (auth_mode %d)", s->auth_mode);
    (void)diag_save();
    return s;
}

avcap2_session *avcap2_session_open(void)
{
 avcap2_session *s;
 if(!capture_guard_take(1)){log_line("avcap2: capture reserved by native recorder");return NULL;}
 s=session_open_impl();if(!s)capture_guard_release(1);return s;
}
void avcap2_session_close(avcap2_session *s)
{
    if (!s) return;
    if (s->handle) {
        s->Stop(s->handle);
        s->Close(s->handle);
        s->handle = 0;
    }
    if (s->Term) s->Term();
    free(s->buf);
    free(s);
    capture_guard_release(1);
}

/* Drain only this read's saved format, even if a later read detects a
 * different channel count. No new ReadAudio may overwrite pending data. */
static int drain_pending(avcap2_session *s, int16_t *out, int max_frames,
                         float *peak_abs)
{
    int frames = s->pending_frames, f, c;
    float peak = 0.f;
    const int16_t *si = (const int16_t *)(const void *)s->buf;
    if (frames > max_frames) frames = max_frames;
    for (f = 0; f < frames; f++) {
        for (c = 0; c < 2; c++) {
            int idx = (s->pending_offset + f) * s->pending_nch + c;
            float v = s->pending_s16 ? (float)si[idx] / 32768.f : s->buf[idx];
            float a;
            if (!(v == v) || v > 1e30f || v < -1e30f) {
                s->nonfinite++;
                v = 0.f;
            }
            if (v > 1.f) v = 1.f;
            if (v < -1.f) v = -1.f;
            a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            out[f * 2 + c] = (int16_t)(v * 32767.f);
        }
    }
    s->pending_offset += frames;
    s->pending_frames -= frames;
    if (!s->pending_frames) s->pending_offset = 0;
    if (peak_abs) *peak_abs = peak;
    return frames;
}

/* In-memory health details are throttled; the log records state changes. */
static void capture_health(avcap2_session *s,long now,int force)
{
    long age=now>=s->last_ok_ms?now-s->last_ok_ms:0;
    if(!force && now-s->last_health_diag_ms<1000)return;
    s->last_health_diag_ms=now;
    diag_set("capture health","%s; no valid PCM for %ld ms; no-wait flag 1; recovery %d/%d (total %llu); last error %#x; %s",
             s->failed?"FAILED":s->recovering?"recovering":s->awaiting_pcm?"awaiting PCM":s->problem_active?"transient error":"active",age,s->recoveries,RECOVERY_LIMIT,
             (unsigned long long)s->recoveries_total,(unsigned)s->last_error,
             s->recovery_reason?s->recovery_reason:"no recovery requested");
    diag_set("capture counters","valid %llu empty %llu zero-length %llu overrun %llu unmapped %llu invalid %llu other %llu no-data %llu",
             (unsigned long long)s->reads_ok,(unsigned long long)s->reads_empty,
             (unsigned long long)s->reads_zero,(unsigned long long)s->reads_overrun,
             (unsigned long long)s->reads_unmapped,(unsigned long long)s->reads_invalid,
             (unsigned long long)s->reads_other,(unsigned long long)s->no_data_events);
}

/* One restart attempt per call, >=1 s between completions and the next
 * attempt. No sleep or polling loop here. IPC calls themselves are synchronous.
 * Three attempts per incident; 30 s of valid capture replenishes the budget. */
static int recover_capture(avcap2_session *s,long now)
{
    int rv;
    if(s->failed)return -1;
    if(s->recoveries>=RECOVERY_LIMIT){
        s->failed=1;
        log_line("avcap2: recovery exhausted after %d attempts (last error %#x, %s)",s->recoveries,
                 (unsigned)s->last_error,s->recovery_reason?s->recovery_reason:"unknown");
        diag_set("audio recovery","FAILED after %d attempts; %s; last error %#x",s->recoveries,
                 s->recovery_reason?s->recovery_reason:"unknown",(unsigned)s->last_error);
        capture_health(s,now,1);
        return -1;
    }
    if(now<s->recover_at_ms){capture_health(s,now,0);return 0;}
    s->recoveries++;s->recoveries_total++;
    s->pending_frames=s->pending_offset=0;
    log_line("avcap2: recovery %d/%d total=%llu reason=%s error=%#x (read flag stays 1)",
             s->recoveries,RECOVERY_LIMIT,(unsigned long long)s->recoveries_total,
             s->recovery_reason?s->recovery_reason:"unknown",(unsigned)s->last_error);
    rv=session_restart(s->Init,s->Term,s->Open,s->Close,s->Start,s->Stop,&s->handle,s->auth_mode);
    now=now_ms();
    s->recover_at_ms=now+RECOVERY_GAP_MS;
    if(rv!=0){
        log_line("avcap2: recovery attempt failed %#x",(unsigned)rv);
        diag_set("audio recovery","attempt %d/%d failed %#x; %s",s->recoveries,RECOVERY_LIMIT,
                 (unsigned)rv,s->recovery_reason?s->recovery_reason:"unknown");
        if(s->recoveries>=RECOVERY_LIMIT){s->failed=1;capture_health(s,now,1);return -1;}
        capture_health(s,now,1);return 0;
    }
    s->recovering=0;
    s->problem_active=s->healthy_active=0;
    s->fmt_s16=s->fmt_checked=0;s->nch=CHANNELS;
    s->rate_t0=0;s->rate_bytes=0;s->rate_done=0;
    s->watchdog_since_ms=now;
    s->awaiting_pcm=1;
    diag_set("audio recovery","reopened %d/%d; awaiting valid PCM; read flag 1",s->recoveries,RECOVERY_LIMIT);
    log_line("avcap2: recovery reopened capture; awaiting PCM");
    capture_health(s,now,1);
    return 0;
}

static int request_recovery(avcap2_session *s,long now,uint32_t error,const char *reason)
{
    s->recovering=1;
    s->healthy_active=s->problem_active=0;
    s->last_error=error;s->recovery_reason=reason;
    s->pending_frames=s->pending_offset=0;
    return recover_capture(s,now);
}

/* Allow one-off invalid/unknown results to clear. Repeated errors cannot
 * turn into silence forever. Their numeric meaning is not guessed. */
static int capture_problem(avcap2_session *s,long now,uint32_t error,const char *reason)
{
    s->healthy_active=0;
    if(!s->problem_active){
        s->problem_active=1;s->problem_since_ms=now;
        log_line("avcap2: capture problem %#x (%s)",(unsigned)error,reason);
    }
    s->last_error=error;
    if(now-s->problem_since_ms>=PROBLEM_GRACE_MS)
        return request_recovery(s,now,error,reason);
    capture_health(s,now,0);
    return 0;
}

static int capture_no_data(avcap2_session *s,long now,uint32_t error)
{
    s->problem_active=0;
    if(now-s->watchdog_since_ms>=NO_DATA_TIMEOUT_MS){
        s->no_data_events++;
        log_line("avcap2: no-data watchdog: %ld ms without valid PCM",now-s->last_ok_ms);
        return request_recovery(s,now,error,"no PCM for 5 seconds");
    }
    capture_health(s,now,0);
    return 0;
}

int avcap2_session_read_s16(avcap2_session *s, int16_t *out, int max_frames,
                            float *peak_abs)
{
    unsigned char meta[META_SIZE];
    int got, frames;
    long t_before, now;
    float peak = 0.f;
    extern long now_ms(void);

    if (peak_abs) *peak_abs = 0.f;
    if (!s || !out || max_frames <= 0) return -1;
    if (s->failed) return -1;
    if (s->recovering) return recover_capture(s, now_ms());
    if (s->pending_frames > 0)
        return drain_pending(s, out, max_frames, peak_abs);
    fill_meta(meta);
    if (!s->logged_first) {
        log_line("avcap2: before first stream ReadAudio (buflen=%#x meta=%#x flag=%d)",
                 READ_BYTES, META_SIZE, AVCAP2_READ_NOWAIT);
        s->logged_first = 1;
    }
    t_before = now_ms();
    got = s->Read(s->handle, s->buf, (size_t)READ_BYTES, meta, AVCAP2_READ_NOWAIT);
    now = now_ms();
    if (now - t_before > 200)
        log_line("avcap2: ReadAudio blocked %ld ms (rv=%#x)", now - t_before,
                 (unsigned)got);

    if (got > 0) {
        if (got > READ_BYTES) {
            s->reads_other++;
            return capture_problem(s,now,(uint32_t)got,"invalid positive byte count");
        }
        if (!s->fmt_checked) {
            /* Float32 audio in [-1,1] has almost every word either 0 or with
             * an exponent near 0x3F/0x3E/0x3C... Int16 pairs read as float
             * give huge / NaN / denormal words. Count implausible words. */
            const uint32_t *w = (const uint32_t *)(void *)s->buf;
            int nw = got / 4, bad = 0, nz = 0, k;
            for (k = 0; k < nw; k++) {
                uint32_t e = (w[k] >> 23) & 0xFF;
                if (!w[k] || w[k] == 0x80000000u) continue;
                nz++;
                if (e == 0xFF || e > 0x7F || e < 0x50) bad++;
            }
            hexdump_n("first PCM",(const unsigned char *)s->buf,got<16?got:16);
            if (nz >= 64) {
                s->fmt_checked = 1;
                s->fmt_s16 = (bad * 4 > nz);   /* >25% implausible floats */
                log_line("avcap2: format check: %d/%d non-zero words implausible "
                         "as float -> %s", bad, nz, s->fmt_s16 ? "int16" : "float32");
            }
        }
        if (!s->rate_done) {
            /* Bytes per second tells the channel count: 48 kHz x 2 ch x 4 B
             * = 384000 B/s; 8 ch float would be ~1.5 MB/s. */
            if (!s->rate_t0) s->rate_t0 = now;
            s->rate_bytes += (uint64_t)got;
            if (now - s->rate_t0 >= 2000) {
                double bps = (double)s->rate_bytes * 1000.0 / (double)(now - s->rate_t0);
                double per = (double)SAMPLE_RATE * (s->fmt_s16 ? 2.0 : 4.0);
                double r = bps / per;
                int ch = (int)(r + 0.5);
                double dev = r - (double)ch;
                /* Only trust a clean multiple (a backlog burst is not). */
                int newch = ((ch == 4 || ch == 6 || ch == 8) &&
                             dev < 0.15 && dev > -0.15) ? ch : 2;
                s->rate_done = 1;
                log_line("avcap2: capture rate %.0f B/s = %.2f ch of %s at %d Hz -> "
                         "using %d ch (L/R = first two)", bps, bps / per,
                         s->fmt_s16 ? "int16" : "float32", SAMPLE_RATE, newch);
                s->nch = newch;
            }
        }
        s->pending_nch = s->nch > 0 ? s->nch : CHANNELS;
        s->pending_s16 = s->fmt_s16;
        s->pending_offset = 0;
        {
            int frame_bytes=s->pending_nch*(s->fmt_s16?2:4);
            if(frame_bytes<4 || got<frame_bytes || got%frame_bytes){
                s->reads_other++;
                return capture_problem(s,now,(uint32_t)got,"unaligned PCM byte count");
            }
            s->pending_frames=got/frame_bytes;
        }
        frames = drain_pending(s, out, max_frames, &peak);
        if (s->reads_ok < 3)
            log_line("avcap2: layout bytes=%d format=%s channels=%d frames=%d pending=%d "
                     "meta=%02x%02x%02x%02x %02x%02x%02x%02x "
                     "%02x%02x%02x%02x %02x%02x%02x%02x",
                     got, s->fmt_s16 ? "s16" : "f32", s->pending_nch, frames, s->pending_frames,
                     meta[0], meta[1], meta[2], meta[3], meta[4], meta[5], meta[6], meta[7],
                     meta[8], meta[9], meta[10], meta[11], meta[12], meta[13], meta[14], meta[15]);
        s->last_ok_ms = now;
        s->watchdog_since_ms=now;
        s->awaiting_pcm=0;
        s->problem_active=0;
        if(!s->healthy_active){s->healthy_active=1;s->healthy_since_ms=now;}
        if(s->recoveries && now-s->healthy_since_ms>=HEALTHY_RESET_MS){
            log_line("avcap2: 30 s of healthy capture; recovery budget replenished");
            s->recoveries=0;
            diag_set("audio recovery","healthy capture; recovery budget restored; total %llu",(unsigned long long)s->recoveries_total);
        }
        s->reads_ok++;
        capture_health(s,now,0);
        if (s->reads_ok <= 3 || (s->reads_ok % 50) == 0)
            log_line("avcap2: Read ok #%llu frames=%d peak=%.4f bytes=%d nonfinite=%llu",
                     (unsigned long long)s->reads_ok, frames, (double)peak, got,
                     (unsigned long long)s->nonfinite);
        if (peak_abs) *peak_abs = peak;
        return frames;
    }
    if((unsigned)got==AVCAP2_EMPTY || got==0){
        if(!got)s->reads_zero++;else s->reads_empty++;
        return capture_no_data(s,now,(uint32_t)got);
    }
    if((unsigned)got==AVCAP2_OVERRUN){
        s->reads_overrun++;s->healthy_active=0;
        if(s->reads_overrun<=3 || s->reads_overrun%100==0)
            log_line("avcap2: overrun #%llu; reader resynchronised by library",(unsigned long long)s->reads_overrun);
        return capture_no_data(s,now,(uint32_t)got);
    }
    if((unsigned)got==AVCAP2_UNMAPPED){
        s->reads_unmapped++;
        return request_recovery(s,now,(uint32_t)got,"shared mapping unavailable");
    }
    if((unsigned)got==AVCAP2_INVALID){
        s->reads_invalid++;
        return capture_problem(s,now,(uint32_t)got,"invalid capture state or buffer");
    }
    s->reads_other++;
    return capture_problem(s,now,(uint32_t)got,"unclassified capture status");
}

/* Live-edge drain at startup/after a stall. Bounded to 16 no-wait reads.
 * A zero read does not overwrite PCM, so the newest complete block remains. */
int avcap2_session_read_latest_s16(avcap2_session *s,int16_t *out,int max_frames,float *peak,unsigned *skipped){
 int latest=0;float latest_peak=0;
 if(skipped)*skipped=0;
 for(int i=0;i<16;i++){
  float p=0;int n=avcap2_session_read_s16(s,out,max_frames,&p);
  if(n<0)return -1;
  if(!n)break;
  if(latest && skipped)*skipped+=(unsigned)latest;
  latest=n;latest_peak=p;
 }
 if(peak)*peak=latest_peak;
 return latest;
}
