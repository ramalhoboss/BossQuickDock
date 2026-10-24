/* hci_usb.c - USB HCI transport (Core spec Vol 4 Part B) over ugen(4).
 *
 * Design notes
 *  - The ugen "fs" interface gives us a table of transfer slots. Each slot
 *    is bound to one endpoint and carries one transfer at a time, so several
 *    slots on the same endpoint give several reads in flight.
 *  - Commands use the synchronous USB_DO_REQUEST ioctl on endpoint 0.
 *  - Received packets land in fixed rings; when a ring is full the oldest
 *    entry is overwritten so fresh traffic is never lost to stale traffic.
 *  - Outgoing ACL frames wait in a ring as well and are handed to the bulk
 *    OUT slot one at a time. A frame that makes no progress for
 *    STALL_SWITCH_MS moves the transmitter onto the spare bulk OUT pipe. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>

#include <ps5/kernel.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include "acl_track.h"
#include "hci_usb.h"
#include "hcidbg.h"
#include "usb_hci_desc.h"
#include "hci_cmd.h"
#include "log.h"
#include "diag.h"
#include "util.h"
#include "stop.h"
#include "btchip.h"
#include "txpath.h"

enum {
    RING_DEPTH      = 64,
    /* The system stack reads the same endpoints; whoever has a transfer
     * pending gets the packet. Few reads in flight (1.0.0-1.0.2 used 4/4)
     * let it take our L2CAP signalling (e.g. the peer's CFG_REQ). Earlier
     * working builds kept 40 event / 20 ACL reads in flight. Pending
     * transfers are served in order, so the system still takes about
     * 1/(ours + 1) of the events (~2.7% measured on a MediaTek 0e8d:3603;
     * lost Number Of Completed Packets are covered by the timed refill).
     * 62 slots is the ceiling: on fw 13.60 the 63rd FS_OPEN fails with
     * ENOMEM, so more event reads mean fewer ACL reads. */
    READS_EVT       = 40,
    READS_ACL       = 20,
    STALL_SWITCH_MS = 500,
    OP_DISCONNECT   = 0x0406,
    DEF_EVT_EP      = 0x82,
    DEF_ACLIN_EP    = 0x81,
    DEF_ACLOUT_EP   = 0x01,
    DEF_MPS         = 16,
    EVT_BUF         = 260,           /* 2-byte header + 255 parameters */
};

/* Slot table layout: event reads, then ACL reads, then the two writers. */
#define SLOT_EVT0   0
#define SLOT_ACL0   (SLOT_EVT0 + READS_EVT)
#define SLOT_OUT    (SLOT_ACL0 + READS_ACL)
#define SLOT_SPARE  (SLOT_OUT + 1)
#define SLOT_COUNT  (SLOT_SPARE + 1)

struct pkt { uint16_t len; unsigned char data[HCI_PKT_MAX]; };

struct ring {
    struct pkt item[RING_DEPTH];
    unsigned   head, count;   /* head = oldest */
    unsigned   overwritten;
};

struct slot {
    int       open, busy;
    uint8_t   addr;
    void     *bufp;
    uint32_t  len;
    unsigned char buf[HCI_PKT_MAX];
};

void (*hci_usb_conn_req_hook)(hci_t hci, const unsigned char *ev, int n);

/* Connection Requests seen in one reap, answered after it (commands are not
 * sent from inside the completion loop). */
#define CREQ_MAX 4
static unsigned char g_creq[CREQ_MAX][16];
static int g_ncreq;

struct usb_hci {
    int    fd;
    int    dead;
    char   node[32];
    struct usbhci_iface ifc;
    struct usb_fs_endpoint fsep[SLOT_COUNT];
    struct slot sl[SLOT_COUNT];
    int    tx_slot;              /* SLOT_OUT or SLOT_SPARE */
    long   tx_since;             /* when the in-flight frame was started */
    hb_txpath tp;                /* spare pipe use (txpath.h) */
    struct ring evq, aclq, txq;
    /* counters for diag */
    unsigned long n_evt, n_acl_in, n_acl_out, n_cmd, n_err, n_switch, n_rearm;
    int    last_status;
};

/* The open transport, so it is released on any exit path (atexit). */
static struct usb_hci *g_open;

/* ---- rings ------------------------------------------------------------ */

static void ring_put(struct ring *r, const unsigned char *p, int n)
{
    struct pkt *dst;
    if (n <= 0) return;
    if (n > HCI_PKT_MAX) n = HCI_PKT_MAX;
    if (r->count == RING_DEPTH) {            /* evict the oldest */
        r->head = (r->head + 1) % RING_DEPTH;
        r->count--;
        r->overwritten++;
    }
    dst = &r->item[(r->head + r->count) % RING_DEPTH];
    dst->len = (uint16_t)n;
    memcpy(dst->data, p, (size_t)n);
    r->count++;
}

static const struct pkt *ring_peek(const struct ring *r)
{
    return r->count ? &r->item[r->head] : NULL;
}

static void ring_drop(struct ring *r)
{
    if (!r->count) return;
    r->head = (r->head + 1) % RING_DEPTH;
    r->count--;
}

/* Remove every frame for ACL handle h, keeping the order of the rest.
 * keep_head: the oldest frame is in flight on the writer, leave it. */
static int ring_drop_handle(struct ring *r, int h, int keep_head)
{
    unsigned i, o = 0, n = r->count;
    int gone = 0;
    for (i = 0; i < n; i++) {
        struct pkt *p = &r->item[(r->head + i) % RING_DEPTH];
        if ((i || !keep_head) && hb_acl_frame_handle(p->data, p->len) == h) { gone++; continue; }
        if (o != i) r->item[(r->head + o) % RING_DEPTH] = *p;
        o++;
    }
    r->count = o;
    return gone;
}

static int ring_take(struct ring *r, unsigned char *dst, int cap)
{
    const struct pkt *p = ring_peek(r);
    int n;
    if (!p || !dst || cap <= 0) return 0;
    n = p->len < cap ? p->len : cap;
    memcpy(dst, p->data, (size_t)n);
    ring_drop(r);
    return n;
}

/* ---- ugen fs plumbing --------------------------------------------------- */

static int slot_bind(struct usb_hci *u, int idx, uint8_t addr)
{
    struct usb_fs_open op;
    memset(&op, 0, sizeof op);
    op.ep_index   = (uint8_t)idx;
    op.ep_no      = addr;
    op.max_bufsize = HCI_PKT_MAX;
    op.max_frames = 1;
    if (ioctl(u->fd, USB_FS_OPEN, &op) < 0) {
        log_line("hci_usb: FS_OPEN slot %d ep 0x%02x: %s", idx, addr,
                 strerror(errno));
        return 0;
    }
    u->sl[idx].open = 1;
    u->sl[idx].addr = addr;
    u->sl[idx].bufp = u->sl[idx].buf;
    u->fsep[idx].ppBuffer = &u->sl[idx].bufp;
    u->fsep[idx].pLength  = &u->sl[idx].len;
    u->fsep[idx].nFrames  = 1;
    return 1;
}

static int slot_go(struct usb_hci *u, int idx, uint32_t len, uint16_t flags)
{
    struct usb_fs_start st;
    struct slot *s = &u->sl[idx];
    if (!s->open || s->busy) return 0;
    s->len = len;
    u->fsep[idx].nFrames = 1;
    u->fsep[idx].aFrames = 0;
    u->fsep[idx].flags   = flags;
    u->fsep[idx].timeout = USB_FS_TIMEOUT_NONE;
    u->fsep[idx].status  = 0;
    st.ep_index = (uint8_t)idx;
    if (ioctl(u->fd, USB_FS_START, &st) < 0) {
        u->n_err++;
        if (errno == ENXIO || errno == ENODEV) u->dead = 1;
        return 0;
    }
    s->busy = 1;
    return 1;
}

/* Never cancel a transfer (USB_FS_STOP): cancelling one the xHCI already
 * holds leaves the system's Bluetooth stuck (pad reconnects included) until
 * a reboot, even with CLEAR_STALL_SYNC after it (tools/usbread_test). A read
 * we no longer want is instead left to finish and simply not re-armed; event
 * reads that must go now are completed on purpose with HCI Read Local
 * Version answers, which harms nothing (tools/usbdrain_test). */
enum { OP_DRAIN = 0x1001 };
static int g_draining;          /* no re-arming while set */
static int g_drain_out;         /* drain commands still unanswered */
static int reap(struct usb_hci *u);
static void tx_kick(struct usb_hci *u);
static int send_cmd(struct usb_hci *u, unsigned op, const void *args, int nargs);

static int busy_in(const struct usb_hci *u, int from, int to)
{
    int k, n = 0;
    for (k = from; k < to; k++) n += u->sl[k].busy;
    return n;
}

/* Completes every pending event read beyond the first `keep`, without
 * cancelling. ACL reads cannot be completed this way: they finish with the
 * next ACL data and are not re-armed. */
static void drain_events(struct usb_hci *u, int keep, int max_ms)
{
    long end = now_ms() + max_ms;
    int k, extra, sent = 0;
    for (;;) {
        extra = 0;
        for (k = SLOT_EVT0 + keep; k < SLOT_ACL0; k++) extra += u->sl[k].busy;
        if (!extra || u->dead || now_ms() >= end) break;
        /* the system's own reads may take some answers: send more as needed */
        if (g_drain_out < extra && sent < 3 * READS_EVT && send_cmd(u, OP_DRAIN, NULL, 0)) {
            g_drain_out++;
            sent++;
        }
        g_draining = 1;
        (void)reap(u);
        g_draining = 0;
        usleep(3000);
    }
    if (extra)
        log_line("hci_usb: %d event read(s) still pending after %d drain command(s)", extra, sent);
    else if (sent)
        log_line("hci_usb: event reads completed with %d drain command(s), none cancelled", sent);
    if (now_ms() >= end) g_drain_out = 0;   /* answers lost to the system: stop expecting them */
}

/* ACL reads are completed the same way: an L2CAP Echo Request to our own
 * link is answered with one ACL packet each, which a pending read takes. */
static unsigned char g_own[6];   /* our link's address (see hci_usb_own) */
static int g_have_own;
static unsigned char g_cown[6];  /* last device we paged / accepted (see op_cmd) */
static int g_have_cown;
/* Handles of our links, learned here from Connection Complete (acl_track's
 * own-handle field is almost never filled in). */
static int g_own_h = -1, g_cown_h = -1;
static long g_page_until;        /* our page / accept is out until then (see own_conn_pending) */
static unsigned own_handle_any(void)
{
    if (g_have_cown && g_cown_h >= 0) return (unsigned)g_cown_h;
    if (g_have_own && g_own_h >= 0) return (unsigned)g_own_h;
    return 0;
}
/* Connection Complete (ACL, success): remember the handle if it is ours.
 * Any answer to our page, a failed one too, ends the page window: a link
 * closing after that is not taken for ours. */
static void own_conn_event(const unsigned char *ev, int n)
{
    int h;
    if (n >= 11 && ev[0] == 0x03 && g_have_cown && !memcmp(ev + 5, g_cown, 6)) g_page_until = 0;
    if (n < 13 || ev[0] != 0x03 || ev[2] != 0 || ev[11] != 0x01) return;
    h = ((int)ev[3] | ((int)ev[4] << 8)) & 0x0FFF;
    if (g_have_cown && !memcmp(ev + 5, g_cown, 6)) g_cown_h = h;
    if (g_have_own && !memcmp(ev + 5, g_own, 6)) g_own_h = h;
}
static int g_echo_out;
static unsigned char g_echo_id = 0xC0;

static void drain_acl(struct usb_hci *u, int keep, int max_ms)
{
    long end = now_ms() + max_ms;
    unsigned h = own_handle_any();
    int k, extra, sent = 0;
    for (;;) {
        extra = 0;
        for (k = SLOT_ACL0 + keep; k < SLOT_OUT; k++) extra += u->sl[k].busy;
        if (!extra || u->dead || now_ms() >= end) break;
        if (h && g_echo_out < extra && sent < 3 * READS_ACL && u->txq.count < 4) {
            unsigned char f[12] = { (unsigned char)(h & 0xFF), (unsigned char)(((h >> 8) & 0x0F) | 0x20),
                                    8, 0, 4, 0, 1, 0, 0x08, 0, 0, 0 };
            f[9] = g_echo_id++;
            if (g_echo_id < 0xC0) g_echo_id = 0xC0;
            ring_put(&u->txq, f, (int)sizeof f);
            tx_kick(u);
            g_echo_out++;
            sent++;
        }
        g_draining = 1;
        (void)reap(u);
        g_draining = 0;
        usleep(3000);
    }
    if (extra)
        log_line("hci_usb: %d ACL read(s) still pending after %d echo(es)%s", extra, sent,
                 h ? "" : " (no link of ours to echo on)");
    else if (sent)
        log_line("hci_usb: ACL reads completed with %d echo(es), none cancelled", sent);
    if (now_ms() >= end) g_echo_out = 0;
}

/* #34 share mode. ugen cannot peek: a read we keep armed takes the next
 * packet off the pipe whatever its handle or address, and nothing can hand it
 * back to the system stack. So "only our link's traffic" is approximated by
 * keeping as few reads armed as our own link needs right now; the system's
 * own pending reads then take the rest (the DualSense's HID reports, its
 * link events and connection requests):
 *   - our command just sent (HB_FULL_MS), or connecting / pairing / a saved
 *     headset rejoining (sticky): every event read; ACL reads in full only
 *     while connecting (L2CAP / AVDTP setup needs every packet),
 *   - streaming: SHARE_EVT event and SHARE_ACL ACL reads,
 *   - idle (no link of ours): SHARE_EVT event reads, no ACL read at all.
 * Lost Number Of Completed Packets are covered by the timed refill. */
enum { SHARE_EVT = 4, SHARE_ACL = 2, HB_FULL_MS = 8000 };
static long g_full_until;
static int  g_hint_stream, g_hint_busy, g_hint_sticky;
static int  g_mode_evt = -1, g_mode_acl = -1;   /* reads armed per kind now */
static long g_yield_until2;               /* mirror of the yield (declared before reap) */

/* Address of our own link (headset streaming or being connected), so a
 * Disconnection Complete for any other handle is known to be someone
 * else's (a pad) and we step aside before it calls back. */

/* Set by the transport itself when we page or accept a device (Create /
 * Accept Connection): main's hint can lag (an incoming headset is accepted
 * before it is the target), and with no ACL read armed its L2CAP / SDP
 * setup went to the system and timed out. */
enum { HB_CONN_MS = 30000 };
static long g_conn_until;

/* Our page / accept is out and its Connection Complete not read yet. The
 * system can read that event instead (shared pipe) and hang up on a link it
 * never asked for: a link closing now is most likely ours, not a pad's. */
enum { HB_PAGE_MS = 10000 };
static int own_conn_pending(void)
{
    return g_have_cown && g_cown_h < 0 && now_ms() < g_page_until;
}

static int is_own_handle(int h)
{
    if (g_have_own && g_own_h >= 0 && h == g_own_h) return 1;
    if (g_have_own && (unsigned)h == acl_track_any_handle(g_own)) return 1;
    return g_have_cown && g_cown_h >= 0 && h == g_cown_h;
}

void hci_usb_own(const unsigned char addr[6])
{
    if (!addr || memcmp(addr, g_own, 6)) {
        g_own_h = -1;
        if (addr && g_have_cown && !memcmp(addr, g_cown, 6)) g_own_h = g_cown_h;
    }
    g_have_own = addr != NULL;
    if (addr) memcpy(g_own, addr, 6);
}

void hci_usb_hint(int streaming, int connecting, int sticky)
{
    g_hint_stream = streaming;
    /* Once streaming the link is up: "connecting" (a target is still set)
     * and the rejoin / listen window must not keep every read armed, or a
     * pad that drops and calls back loses its pairing packets to us for
     * the whole stream and hangs (white light, no reconnect). */
    g_hint_busy = connecting && !streaming;
    g_hint_sticky = sticky && !streaming;
}

static void want_counts(int *ne, int *na)
{
    int full = now_ms() < g_full_until;
    int conn = now_ms() < g_conn_until && !g_hint_stream;
    if (now_ms() < g_yield_until2) { *ne = *na = 0; return; }
    if (conn) { *ne = READS_EVT; *na = READS_ACL; return; }
    /* Idle (no link of ours, nothing connecting): no read at all, so a pad
     * that drops and calls back reaches the system with every packet. Any
     * read kept armed here could take its Connection Request, which cannot
     * be handed back and leaves the pad unable to reconnect. */
    if (!full && !g_hint_busy && !g_hint_sticky && !g_hint_stream) { *ne = *na = 0; return; }
    /* Our own command (a periodic quality check, say) needs only a few event
     * reads: going to every read and back meant draining ~40 of them with as
     * many extra commands every 10 s, which stalled the audio and took
     * system replies (pad reconnects then hung). Every read only while
     * connecting / rejoining. */
    /* Streaming keeps every read too: with few event reads most Number Of
     * Completed Packets went to the system and the stream starved of
     * credits (stutter). A pad is handled by the yield instead, which
     * empties every read (drained, never cancelled) while it reconnects. */
    *ne = (g_hint_busy || g_hint_sticky || g_hint_stream) ? READS_EVT : SHARE_EVT;
    *na = (g_hint_busy || g_hint_stream) ? READS_ACL : 0;
}

static int slot_wanted(int idx)
{
    int ne, na;
    want_counts(&ne, &na);
    if (idx >= SLOT_EVT0 && idx < SLOT_ACL0) return idx - SLOT_EVT0 < ne;
    if (idx >= SLOT_ACL0 && idx < SLOT_OUT) return idx - SLOT_ACL0 < na;
    return 1;
}

static void arm_read(struct usb_hci *u, int idx)
{
    if (g_draining || !slot_wanted(idx)) return;
    (void)slot_go(u, idx, HCI_PKT_MAX, USB_FS_FLAG_SINGLE_SHORT_OK);
}

/* Applies the current counts. Extra reads are never cancelled: extra event
 * reads are drained (so idle really reads nothing), extra ACL reads finish
 * with their next packet and are not re-armed. */
static void share_step(struct usb_hci *u)
{
    int ne, na;
    want_counts(&ne, &na);
    if (ne == g_mode_evt && na == g_mode_acl) return;
    g_mode_evt = ne;
    g_mode_acl = na;
    if (busy_in(u, SLOT_EVT0 + ne, SLOT_ACL0)) drain_events(u, ne, 200);
    if (busy_in(u, SLOT_ACL0 + na, SLOT_OUT)) drain_acl(u, na, 400);
    log_line("hci_usb: reads armed: %d event / %d ACL (%s)", ne, na,
             now_ms() < g_conn_until && !g_hint_stream ? "link setup" :
             now_ms() < g_full_until ? "our command" : g_hint_busy ? "connecting" :
             g_hint_sticky ? "headset rejoin" : g_hint_stream ? "streaming, shared" : "idle, shared");
}

/* Hand the oldest queued ACL frame to the active writer, if it is idle. */
static void tx_kick(struct usb_hci *u)
{
    const struct pkt *p;
    struct slot *w = &u->sl[u->tx_slot];
    if (w->busy || !(p = ring_peek(&u->txq))) return;
    memcpy(w->buf, p->data, p->len);
    if (slot_go(u, u->tx_slot, p->len, USB_FS_FLAG_FORCE_SHORT))
        u->tx_since = now_ms();
}

/* Back onto the primary bulk OUT; the frame in flight on the spare (if
 * any) stays at the head of txq and goes out again on the primary. */
static int g_tx_back;   /* switch back once the spare's write finishes */
static void tx_primary(struct usb_hci *u, const char *why)
{
    if (u->tx_slot == SLOT_OUT) return;
    /* never cancel the write in flight: wait for it (reap calls back) */
    if (u->sl[SLOT_SPARE].busy) { g_tx_back = 1; return; }
    g_tx_back = 0;
    u->tx_slot = SLOT_OUT;
    log_line("hci_usb: back on bulk OUT 0x%02x (%s)", u->sl[SLOT_OUT].addr, why);
    tx_kick(u);
}

static void tx_watchdog(struct usb_hci *u)
{
    if (hb_txpath_check(&u->tp, now_ms())) {
        tx_primary(u, "the spare pipe completed nothing, not using it again");
        return;
    }
    if (!u->sl[u->tx_slot].busy) return;
    if (now_ms() - u->tx_since < STALL_SWITCH_MS) return;
    if (u->tx_slot != SLOT_OUT) return;
    if (!hb_txpath_stall(&u->tp, u->sl[SLOT_SPARE].open, now_ms())) return;
    log_line("hci_usb: bulk OUT 0x%02x stuck %d ms, moving to 0x%02x",
             u->sl[SLOT_OUT].addr, STALL_SWITCH_MS, u->sl[SLOT_SPARE].addr);
    /* The stuck write is left pending, not cancelled; the frame goes again
     * on the spare (a duplicate if the old one ever completes, which the
     * controller tolerates far better than a cancel). */
    u->tx_slot = SLOT_SPARE;
    u->n_switch++;
    tx_kick(u);   /* the same frame is still at the head of txq */
}

/* Drain every finished transfer. Returns 1 if new input was queued. */
/* #34 yield state (see hci_usb_yield). */
static long g_yield_until;
static int  g_yielding;
static unsigned long g_yields;

/* #34 debug: ACL frames we took, per handle, logged every 10 s. A handle
 * that is not our headset's is traffic the system lost (a pad's HID). */
static int  g_acl_h[4], g_acl_c[4];
static long g_acl_t0;
static void acl_in_note(int h)
{
    int i;
    long now = now_ms();
    for (i = 0; i < 4; i++) {
        if (g_acl_c[i] && g_acl_h[i] == h) { g_acl_c[i]++; break; }
        if (!g_acl_c[i]) { g_acl_h[i] = h; g_acl_c[i] = 1; break; }
    }
    if (!g_acl_t0) g_acl_t0 = now;
    if (now - g_acl_t0 >= 10000) {
        log_line("hci_usb: ACL in taken in %ld ms: %#05x x%d, %#05x x%d, %#05x x%d, %#05x x%d (%s)",
                 now - g_acl_t0, g_acl_h[0], g_acl_c[0], g_acl_h[1], g_acl_c[1],
                 g_acl_h[2], g_acl_c[2], g_acl_h[3], g_acl_c[3],
                 g_mode_acl >= READS_ACL ? "full" : "shared");
        memset(g_acl_c, 0, sizeof g_acl_c);
        g_acl_t0 = now;
    }
}

/* Our commands still waiting for an answer. The system stack sends its own
 * commands on the same controller and waits for each answer before the
 * next one: an answer we take is lost to it, and its command queue (pad
 * reconnects included) hangs until a reboot. So every Command Complete /
 * Status we read is checked against what we sent, and the full-read window
 * closes as soon as our own command is answered. */
/* Replay of a system reply we took. The system's stack waits for that
 * opcode's Command Complete / Status for good (pad reconnects included), so
 * once every read of ours is empty the same opcode is sent again with an
 * invalid handle / address: the controller rejects it, with no effect on
 * any link, and that reply (same opcode) can only reach the system. */
static unsigned g_replay_op;
static int g_replay_tries;

/* Parameter length for a harmless (rejected) copy of op, -1 if no safe one. */
static int replay_len(unsigned op)
{
    switch (op) {
    case 0x0406: return 3;   case 0x0409: return 7;   case 0x040a: return 7;
    case 0x040b: return 22;  case 0x040c: return 6;   case 0x0411: return 2;
    case 0x0413: return 3;   case 0x0419: return 10;  case 0x041a: return 6;
    case 0x041b: return 2;   case 0x041c: return 3;   case 0x041d: return 2;
    case 0x041f: return 2;   case 0x042b: return 9;   case 0x042c: return 6;
    case 0x042d: return 6;   case 0x0434: return 6;
    case 0x0803: return 10;  case 0x0804: return 2;   case 0x0807: return 21;
    case 0x0809: return 2;   case 0x080b: return 7;   case 0x080c: return 2;
    case 0x080d: return 4;   case 0x0811: return 8;
    case 0x1403: return 2;   case 0x1405: return 2;   case 0x1407: return 3;
    }
    if (op >= 0x1001 && op <= 0x100F) return 0;   /* informational reads: no params */
    return -1;
}

static void replay_step(struct usb_hci *u)
{
    unsigned char a[32];
    int n;
    if (!g_replay_op) return;
    if (busy_in(u, SLOT_EVT0, SLOT_ACL0)) {          /* the answer must not reach us */
        if (++g_replay_tries > 50) { log_line("hci_usb: replay of %#06x given up (reads still pending)", g_replay_op); g_replay_op = 0; }
        return;
    }
    n = replay_len(g_replay_op);
    memset(a, 0xFF, sizeof a);                       /* handle 0xFFFF / address ff:..:ff: rejected */
    if (n >= 0 && send_cmd(u, g_replay_op, a, n))
        log_line("hci_usb: replayed %#06x (rejected copy) so the system gets its answer", g_replay_op);
    g_replay_op = 0;
}

#define CMD_PEND 8
static unsigned g_cmd_op[CMD_PEND];
static long     g_cmd_t[CMD_PEND];
static unsigned long g_foreign_replies;

static void cmd_sent(unsigned op)
{
    int i, old = 0;
    for (i = 0; i < CMD_PEND; i++) {
        if (!g_cmd_op[i]) { old = i; break; }
        if (g_cmd_t[i] < g_cmd_t[old]) old = i;
    }
    g_cmd_op[old] = op;
    g_cmd_t[old] = now_ms();
}

static void cmd_reply_seen(const unsigned char *ev, int n)
{
    unsigned op;
    int i, ours = 0;
    if (ev[0] == 0x0E && n >= 5) op = (unsigned)ev[3] | ((unsigned)ev[4] << 8);
    else if (ev[0] == 0x0F && n >= 6) op = (unsigned)ev[4] | ((unsigned)ev[5] << 8);
    else return;
    if (!op) return;                       /* NOP: credits only, nobody waits on it */
    for (i = 0; i < CMD_PEND; i++)
        if (g_cmd_op[i] == op && now_ms() - g_cmd_t[i] < 10000) {
            g_cmd_op[i] = 0;
            ours = 1;
            break;
        }
    if (!ours) {
        g_foreign_replies++;
        log_line("hci_usb: WARNING took the system's reply to command %#06x (%s, %lu so far)",
                 op, ev[0] == 0x0E ? "Command Complete" : "Command Status", g_foreign_replies);
        if (op == OP_DRAIN) return;          /* a late drain answer of ours */
        if (replay_len(op) < 0) {
            log_line("hci_usb: no safe replay for %#06x", op);
        } else {
            g_replay_op = op;
            g_replay_tries = 0;
        }
        /* the system is busy with something (a pad, most likely): step aside.
         * Its Disconnect while our page is out is most likely for our own new
         * link (see reap): off only until the replay is done. */
        if (op == OP_DISCONNECT && own_conn_pending()) {
            log_line("hci_usb: the system hung up a link while our page was out - probably ours");
            hci_usb_yield(HB_YIELD_PAGE_MS);
        } else {
            hci_usb_yield(g_hint_stream ? HB_YIELD_STREAM_MS : HB_YIELD_IDLE_MS);
        }
        return;
    }
    /* Command Complete ends our command: stop reading everything now
     * instead of for HB_FULL_MS. A Command Status means more events follow
     * (inquiry results, connection complete), so the window stays. */
    if (ev[0] == 0x0E) {
        for (i = 0; i < CMD_PEND; i++) if (g_cmd_op[i]) return;
        if (g_full_until > now_ms() + 300) g_full_until = now_ms() + 300;
    }
}

static int reap(struct usb_hci *u)
{
    struct usb_fs_complete c;
    int got = 0;
    for (;;) {
        int idx, st;
        struct slot *s;
        memset(&c, 0, sizeof c);
        if (ioctl(u->fd, USB_FS_COMPLETE, &c) < 0) {
            if (errno == ENXIO || errno == ENODEV) u->dead = 1;
            break;                          /* EBUSY: nothing more */
        }
        idx = c.ep_index;
        if (idx >= SLOT_COUNT) continue;
        s = &u->sl[idx];
        s->busy = 0;
        st = u->fsep[idx].status;
        if (st) { u->n_err++; u->last_status = st; }

        if (idx >= SLOT_EVT0 && idx < SLOT_ACL0) {
            if (!st && s->len >= 6 && g_drain_out > 0 && s->buf[0] == 0x0E &&
                (s->buf[3] | (s->buf[4] << 8)) == OP_DRAIN) {
                g_drain_out--;                 /* our drain answer: nobody wants it */
            } else if (!st && s->len >= 2) {
                int dh = hb_evt_disc_handle(s->buf, (int)s->len);
                if (hb_evt_is_nocp(s->buf, (int)s->len)) hb_txpath_nocp(&u->tp);
                own_conn_event(s->buf, (int)s->len);
                if (dh >= 0 && is_own_handle(dh)) {
                    if (dh == g_own_h) g_own_h = -1;            /* our link is gone */
                    if (dh == g_cown_h) g_conn_until = 0, g_have_cown = 0, g_cown_h = -1;
                } else if (dh >= 0 && own_conn_pending()) {
                    /* A link we never saw come up, closed while our page is
                     * out: most likely ours (its Connection Complete went to
                     * the system). A short pause, then the pairing / btlink
                     * pages again. */
                    log_line("hci_usb: link %#05x closed while our page was out - probably ours, its Connection Complete went to the system", dh);
                    hci_usb_yield(HB_YIELD_PAGE_MS);
                } else if (dh >= 0) {
                    /* Not our link going down (a pad turning off, sleeping
                     * or losing range): its call-back must reach only the
                     * system, so stop reading before it pages again. */
                    log_line("hci_usb: link %#05x (not ours) closed - stepping aside for its reconnect", dh);
                    hci_usb_yield(g_hint_stream ? HB_YIELD_STREAM_MS : HB_YIELD_IDLE_MS);
                }
                if (dh >= 0) {
                    /* The controller freed this handle's buffers: drop what
                     * we still hold for it, and start the next link on the
                     * primary pipe. */
                    /* (a frame already in flight finishes on its own: the
                     * controller takes and discards it) */
                    int gone = ring_drop_handle(&u->txq, dh, u->sl[u->tx_slot].busy);
                    if (gone) log_line("hci_usb: %d queued frame(s) for closed handle %#05x dropped", gone, dh);
                    if (hb_txpath_disc(&u->tp)) tx_primary(u, "link closed");
                    else tx_kick(u);
                }
                cmd_reply_seen(s->buf, (int)s->len);
                acl_track_event(s->buf, (int)s->len, now_ms());   /* every link, any owner */
                if (s->buf[0] == 0x04 && s->len >= 12 && g_ncreq < CREQ_MAX)
                    memcpy(g_creq[g_ncreq++], s->buf, 12);
                ring_put(&u->evq, s->buf, (int)s->len); u->n_evt++; got = 1;
            }
            if (!g_yielding) arm_read(u, idx);
        } else if (idx >= SLOT_ACL0 && idx < SLOT_OUT) {
            if (!st && s->len >= 10 && g_echo_out > 0 && s->buf[6] == 1 && s->buf[7] == 0 &&
                s->buf[8] == 0x09 && s->buf[9] >= 0xC0) {
                g_echo_out--;                  /* our drain echo answered: drop it */
            } else if (!st && s->len >= 4) {
                int ah = hb_acl_frame_handle(s->buf, (int)s->len);
                if (ah >= 0 && own_handle_any() && !is_own_handle(ah) && now_ms() >= g_yield_until2) {
                    log_line("hci_usb: took ACL data of link %#05x (not ours) - stepping aside", ah);
                    hci_usb_yield(30000);         /* a pad still setting up its link */
                }
                acl_in_note(ah);
                ring_put(&u->aclq, s->buf, (int)s->len); u->n_acl_in++; got = 1;
            }
            if (!g_yielding) arm_read(u, idx);
        } else if (idx == u->tx_slot) {
            if (!st) { ring_drop(&u->txq); u->n_acl_out++; hb_txpath_sent(&u->tp); }
            else ring_drop(&u->txq);       /* do not spin on a bad frame */
            if (g_tx_back && idx == SLOT_SPARE) tx_primary(u, "spare write finished");
            else tx_kick(u);
        }
    }
    return got;
}

/* ---- hci_ops ------------------------------------------------------------ */

static int op_next_event(void *self, unsigned char *dst, int cap)
{
    int n = ring_take(&((struct usb_hci *)self)->evq, dst, cap);
    if (n > 0) hcidbg_event(dst, n);
    return n;
}

static int op_next_acl(void *self, unsigned char *dst, int cap)
{
    return ring_take(&((struct usb_hci *)self)->aclq, dst, cap);
}

static const hci_ops usb_ops;

/* Answer (or hand over) every Connection Request read so far, and give the
 * hook its periodic tick (ev NULL). */
static void creq_flush(struct usb_hci *u)
{
    hci_t h;
    int i, n = g_ncreq;
    static int busy;
    if (!hci_usb_conn_req_hook || busy) return;   /* no recursion from the hook's own pumps */
    h.ctx = u;
    h.ops = &usb_ops;
    busy = 1;
    g_ncreq = 0;
    for (i = 0; i < n; i++) hci_usb_conn_req_hook(h, g_creq[i], 12);
    hci_usb_conn_req_hook(h, NULL, 0);
    busy = 0;
}

/* #34: the system stack reads the same pipes, and every read we keep armed
 * takes packets it needs. When a device that is not ours calls in (a pad),
 * its Connection Request was already taken by us and cannot be handed back,
 * so we stop reading for a while: the pad's next page, its Link Key Request,
 * Connection Complete and HID data all reach the system. */

void hci_usb_yield(int ms)
{
    long until = now_ms() + (ms > 0 ? ms : 0);
    if (until > g_yield_until) g_yield_until = until;
    g_yield_until2 = g_yield_until;
    g_yields++;
    log_line("hci_usb: another device is calling the console - reads off for %d ms so the system answers it (#%lu)",
             ms, g_yields);
}

/* 1 while yielding: event reads drained, ACL reads left to finish, nothing re-armed. */
static int yield_step(struct usb_hci *u)
{
    int k;
    /* A replay still waiting keeps the reads off (a short yield can end
     * before every read is empty): its answer must reach only the system. */
    if (g_yielding && g_replay_op && now_ms() >= g_yield_until)
        g_yield_until = g_yield_until2 = now_ms() + 100;
    if (now_ms() < g_yield_until) {
        if (!g_yielding) {
            g_yielding = 1;               /* reap stops re-arming */
            drain_events(u, 0, 400);      /* event reads completed, not cancelled */
            drain_acl(u, 0, 800);         /* ACL reads too: the pad's L2CAP setup is ACL */
        }
        replay_step(u);
        return 1;
    }
    if (g_yielding) {
        g_yielding = 0;
        for (k = SLOT_EVT0; k < SLOT_OUT; k++)
            if (u->sl[k].open && !u->sl[k].busy) arm_read(u, k);
        log_line("hci_usb: reads back on");
    }
    return 0;
}

static int op_pump(void *self, int wait_ms)
{
    struct usb_hci *u = self;
    long until = now_ms() + (wait_ms > 0 ? wait_ms : 0);
    if (u->dead) return -1;
    /* Stop file / signal: report "transport gone" so every loop unwinds
     * to its cleanup path (links closed, USB released). */
    if (hb_stop_requested()) return -1;
    share_step(u);
    while (yield_step(u)) {
        long left;
        (void)reap(u);
        if (u->dead) return -1;
        if (u->evq.count || u->aclq.count) return 1;
        left = until - now_ms();
        if (left <= 0) return 0;
        usleep((useconds_t)(left > 50 ? 50 : left) * 1000);
        if (hb_stop_requested()) return -1;
    }
    for (;;) {
        struct pollfd pf;
        long left;
        int got = reap(u), k;
        creq_flush(u);
        share_step(u);
        if (g_yielding || now_ms() < g_yield_until) return got || u->evq.count || u->aclq.count;
        /* A read whose re-arm failed (EBUSY/transient error) would leave
         * the event or ACL pipe silent for good: re-arm idle readers. */
        for (k = SLOT_EVT0; k < SLOT_OUT; k++)
            if (u->sl[k].open && !u->sl[k].busy) {
                arm_read(u, k);
                if (u->sl[k].busy && !(u->n_rearm++ % 50))
                    log_line("hci_usb: re-armed idle %s read slot %d (%lu so far)",
                             k < SLOT_ACL0 ? "event" : "ACL", k, u->n_rearm);
            }
        tx_watchdog(u);
        tx_kick(u);
        if (u->dead) return -1;
        if (got || u->evq.count || u->aclq.count) return 1;
        left = until - now_ms();
        if (left <= 0) return 0;
        if (left > 50) left = 50;         /* keep the watchdog ticking */
        pf.fd = u->fd; pf.events = POLLIN | POLLOUT | POLLRDNORM; pf.revents = 0;
        if (poll(&pf, 1, (int)left) < 0 && errno != EINTR) { u->dead = 1; return -1; }
        if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) { u->dead = 1; return -1; }
    }
}

static int op_cmd(void *self, unsigned op, const void *args, int nargs)
{
    struct usb_hci *u = self;
    if (u->dead || nargs < 0 || nargs > 255) return 0;
    g_full_until = now_ms() + HB_FULL_MS;   /* #34: our command, its answers need every read */
    if ((op == 0x0405 || op == 0x0409) && nargs >= 6 && args) {
        memcpy(g_cown, args, 6);             /* Create / Accept Connection: bdaddr first */
        g_have_cown = 1;
        g_cown_h = -1;
        g_conn_until = now_ms() + HB_CONN_MS;
        g_page_until = now_ms() + HB_PAGE_MS;
    }
    /* Idle keeps no read armed: arm them before the command goes out, or
     * its Command Complete may reach only the system. */
    share_step(u);
    if (!g_yielding) {
        int k;
        for (k = SLOT_EVT0; k < SLOT_ACL0; k++)
            if (u->sl[k].open && !u->sl[k].busy) arm_read(u, k);
    }
    hcidbg_cmd(op, args, nargs);
    cmd_sent(op);
    return send_cmd(u, op, args, nargs);
}

/* Control-pipe HCI command, no read bookkeeping (drain uses it directly). */
static int send_cmd(struct usb_hci *u, unsigned op, const void *args, int nargs)
{
    unsigned char pkt[3 + 255];
    struct usb_ctl_request rq;
    put16(pkt, op);
    pkt[2] = (unsigned char)nargs;
    if (nargs) memcpy(pkt + 3, args, (size_t)nargs);

    /* Setup stage per Vol 4 Part B 2.2: class, host-to-device, request 0. */
    memset(&rq, 0, sizeof rq);
    {
        unsigned char *setup = (unsigned char *)&rq.ucr_request;
        setup[0] = UT_WRITE_CLASS_DEVICE;            /* 0x20 */
        put16(setup + 4, (unsigned)u->ifc.number);   /* wIndex */
        put16(setup + 6, (unsigned)(3 + nargs));     /* wLength */
    }
    rq.ucr_data = pkt;
    if (ioctl(u->fd, USB_DO_REQUEST, &rq) < 0) {
        int e = errno;
        u->n_err++;
        /* Disconnect of a link that already went away often times out on
         * the control pipe; the controller is fine, so carry on. */
        if (op == OP_DISCONNECT && (e == EIO || e == ETIMEDOUT)) {
            log_line("hci_usb: disconnect request: %s (ignored)", strerror(e));
            return 1;
        }
        log_line("hci_usb: command 0x%04x failed: %s", op, strerror(e));
        if (e == ENXIO || e == ENODEV || e == EIO) u->dead = 1;
        return 0;
    }
    u->n_cmd++;
    return 1;
}

static int op_acl_send(void *self, const unsigned char *frame, int nbytes)
{
    struct usb_hci *u = self;
    if (u->dead || !frame || nbytes < 4 || nbytes > HCI_PKT_MAX) return 0;
    ring_put(&u->txq, frame, nbytes);
    tx_kick(u);
    return 1;
}

static void op_diag(void *self)
{
    const struct usb_hci *u = self;
    log_line("hci_usb diag: %s if=%d evt=0x%02x in=0x%02x out=0x%02x spare=0x%02x tx=%s%s",
             u->node, u->ifc.number, u->ifc.evt_ep, u->ifc.in_ep, u->ifc.out_ep,
             u->ifc.spare_out_ep, u->tx_slot == SLOT_SPARE ? "spare" : "primary",
             u->dead ? " DEAD" : "");
    log_line("hci_usb diag: cmd=%lu evt=%lu acl_in=%lu acl_out=%lu err=%lu last=%d switch=%lu",
             u->n_cmd, u->n_evt, u->n_acl_in, u->n_acl_out, u->n_err,
             u->last_status, u->n_switch);
    log_line("hci_usb diag: queued evt=%u acl=%u tx=%u lost evt=%u acl=%u tx=%u",
             u->evq.count, u->aclq.count, u->txq.count,
             u->evq.overwritten, u->aclq.overwritten, u->txq.overwritten);
}


static void op_close(void *self)
{
    struct usb_hci *u = self;
    struct usb_fs_uninit un;
    int i;
    if (!u) return;
    if (g_open == u) g_open = NULL;
    if (u->fd >= 0) {
        long end;
        /* Nothing is cancelled, closing included (FS_CLOSE on a pending
         * transfer cancels it): drain event reads, give ACL reads and
         * writes a moment to finish, then close what is idle. A pipe still
         * pending is left to the process exit rather than stopped here. */
        g_yielding = 1;
        drain_events(u, 0, 1000);
        drain_acl(u, 0, 1000);
        for (end = now_ms() + 1500; busy_in(u, 0, SLOT_COUNT) && now_ms() < end && !u->dead; ) {
            g_draining = 1;
            (void)reap(u);
            g_draining = 0;
            usleep(5000);
        }
        if (busy_in(u, 0, SLOT_COUNT))
            log_line("hci_usb: %d transfer(s) still pending at close, left open (not cancelled)",
                     busy_in(u, 0, SLOT_COUNT));
        for (i = 0; i < SLOT_COUNT; i++) {
            struct usb_fs_close cl;
            if (!u->sl[i].open || u->sl[i].busy) continue;
            cl.ep_index = (uint8_t)i;
            (void)ioctl(u->fd, USB_FS_CLOSE, &cl);
        }
        if (busy_in(u, 0, SLOT_COUNT)) {
            /* UNINIT / close would cancel them; the kernel still writes
             * into our buffers when they finish, so u is not freed either. */
            log_line("hci_usb: %s kept open until exit (pending transfers)", u->node);
            return;
        }
        memset(&un, 0, sizeof un);
        (void)ioctl(u->fd, USB_FS_UNINIT, &un);
        close(u->fd);
    }
    log_line("hci_usb: released %s", u->node);
    free(u);
}

static void release_at_exit(void)
{
    if (g_open) op_close(g_open);
}

static const hci_ops usb_ops = {
    .next_acl   = op_next_acl,
    .next_event = op_next_event,
    .pump       = op_pump,
    .cmd        = op_cmd,
    .acl_send   = op_acl_send,
    .diag       = op_diag,
    .close      = op_close,
};

/* ---- discovery ------------------------------------------------------------ */

static int read_config(int fd, uint8_t *buf, int cap)
{
    struct usb_gen_descriptor gd;
    memset(&gd, 0, sizeof gd);
    gd.ugd_data = buf;
    gd.ugd_maxlen = (uint16_t)cap;
    gd.ugd_config_index = 0;
    if (ioctl(fd, USB_GET_FULL_DESC, &gd) < 0) return 0;
    return gd.ugd_actlen;
}

/* Returns the number of Bluetooth HCI interfaces (class e0/01/01) in the
 * configuration, or -1 when the descriptor could not be read. */
static int pick_endpoints(int fd, struct usbhci_iface *ifc)
{
    static uint8_t cfg[1024];
    struct usbhci_iface found[USBHCI_MAX_IFACES];
    int n = read_config(fd, cfg, (int)sizeof cfg);
    int k = n > 0 ? usbhci_scan(cfg, n, found) : 0;

    memset(ifc, 0, sizeof *ifc);
    if (k > 0) *ifc = found[0];
    if (!ifc->evt_ep) { ifc->evt_ep = DEF_EVT_EP;    ifc->evt_mps = DEF_MPS; }
    if (!ifc->in_ep)  { ifc->in_ep  = DEF_ACLIN_EP;  ifc->in_mps  = DEF_MPS; }
    if (!ifc->out_ep) { ifc->out_ep = DEF_ACLOUT_EP; ifc->out_mps = DEF_MPS; }
    if (!ifc->evt_mps) ifc->evt_mps = DEF_MPS;
    if (!ifc->in_mps)  ifc->in_mps  = DEF_MPS;
    if (!ifc->out_mps) ifc->out_mps = DEF_MPS;
    if (k > 0 || n <= 0)
        log_line("hci_usb: %s descriptor (%d HCI iface), evt 0x%02x in 0x%02x out 0x%02x",
                 k > 0 ? "using" : "fallback, no", k, ifc->evt_ep, ifc->in_ep, ifc->out_ep);
    return n > 0 ? k : -1;
}

/* No readable configuration: only a device that says it is a wireless
 * controller (class e0) or comes from a PS5 Bluetooth chip vendor. */
static int bt_by_device(int fd, int vid)
{
    struct usb_device_descriptor dd;
    memset(&dd, 0, sizeof dd);
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0 && dd.bDeviceClass == 0xE0) return 1;
    return vid == 0x1286 || vid == 0x0e8d;   /* Marvell, MediaTek (btchip.h) */
}

/* VID:PID of the controller hci_usb_open() picked (-1 until then). */
static int g_chip_vid = -1, g_chip_pid = -1;

int hci_usb_chip(int *vid, int *pid)
{
    if (vid) *vid = g_chip_vid;
    if (pid) *pid = g_chip_pid;
    return g_chip_vid >= 0;
}

/* "1286:2059 \"product\" by \"vendor\"" for an open ugen fd (read-only
 * ioctls: nothing is sent to the device). vid/pid (may be NULL) get the
 * IDs, or -1 when the device descriptor cannot be read. */
static void device_id(int fd, char *out, size_t cap, int *vid, int *pid)
{
    struct usb_device_descriptor dd;
    struct usb_device_info di;
    int n = 0;

    memset(&dd, 0, sizeof dd);
    memset(&di, 0, sizeof di);
    if (vid) *vid = -1;
    if (pid) *pid = -1;
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0) {
        n = snprintf(out, cap, "%04x:%04x class %02x", UGETW(dd.idVendor),
                     UGETW(dd.idProduct), dd.bDeviceClass);
        if (vid) *vid = UGETW(dd.idVendor);
        if (pid) *pid = UGETW(dd.idProduct);
    } else
        n = snprintf(out, cap, "????:???? (device descriptor errno %d)", errno);
    if (n > 0 && (size_t)n < cap && ioctl(fd, USB_GET_DEVICEINFO, &di) == 0)
        snprintf(out + n, cap - (size_t)n, " \"%.40s\" by \"%.40s\"",
                 di.udi_product, di.udi_vendor);
}

/* 1 = opened. 0 = not usable (or not a Bluetooth controller at all).
 * -1 = a Bluetooth controller whose transfer slots are still held, e.g. by
 * an instance that is shutting down: worth another try in a moment. */
static int try_node(struct usb_hci *u, const char *path)
{
    struct usb_fs_init in;
    char id[128], chip[64];
    int i, vid, pid, mtk, nbt, inited = 0, e;
    u->fd = open(path, O_RDWR);
    if (u->fd < 0) {
        log_line("hci_usb: open %s: errno %d", path, errno);
        return 0;
    }
    snprintf(u->node, sizeof u->node, "%s", path);
    device_id(u->fd, id, sizeof id, &vid, &pid);
    log_line("usb: %s is %s", path, id);
    nbt = pick_endpoints(u->fd, &u->ifc);
    if (nbt == 0 || (nbt < 0 && !bt_by_device(u->fd, vid))) {
        /* Never treat a headset dongle, pad or anything else as a Bluetooth
         * controller (#25: the PS Link dongle 054c:0ce6 / 0ecc is audio). */
        log_line("hci_usb: %s is not a Bluetooth controller (no e0/01/01 interface), skipped", path);
        close(u->fd);
        u->fd = -1;
        return 0;
    }
    /* MediaTek (every Pro, some fat and Slim): its fixes only run there. */
    mtk = btchip_profile(vid, btchip_get_override()) == BTCHIP_PROFILE_MEDIATEK;
    log_line("hci_usb: chip profile %s%s", btchip_profile_name(mtk ? BTCHIP_PROFILE_MEDIATEK : BTCHIP_PROFILE_DEFAULT),
             btchip_get_override() >= 0 ? " (override from the chip file)" : "");
    if (mtk) {
        usbhci_pair_out_with_in(&u->ifc);
        log_line("hci_usb: MediaTek controller: ACL out 0x%02x, system scan paused during page/scan",
                 u->ifc.out_ep);
    }

    memset(&in, 0, sizeof in);
    in.pEndpoints = u->fsep;
    in.ep_index_max = SLOT_COUNT;
    if (ioctl(u->fd, USB_FS_INIT, &in) < 0) goto fail;
    inited = 1;

    for (i = 0; i < READS_EVT; i++)
        if (!slot_bind(u, SLOT_EVT0 + i, u->ifc.evt_ep)) goto fail;
    for (i = 0; i < READS_ACL; i++)
        if (!slot_bind(u, SLOT_ACL0 + i, u->ifc.in_ep)) goto fail;
    if (!slot_bind(u, SLOT_OUT, u->ifc.out_ep)) goto fail;
    if (u->ifc.spare_out_ep) (void)slot_bind(u, SLOT_SPARE, u->ifc.spare_out_ep);

    for (i = SLOT_EVT0; i < SLOT_OUT; i++) arm_read(u, i);
    u->tx_slot = SLOT_OUT;
    hb_txpath_init(&u->tp);
    hci_scan_pause_enable(mtk);
    hci_set_mediatek(mtk);
    log_line("hci_usb: opened %s (up to %d event / %d ACL reads)", path,
             READS_EVT, READS_ACL);
    diag_set("bt controller", "%s %s; HCI iface %d evt 0x%02x in 0x%02x out 0x%02x", path, id,
             u->ifc.number, u->ifc.evt_ep, u->ifc.in_ep, u->ifc.out_ep);
    g_chip_vid = vid;
    g_chip_pid = pid;
    btchip_describe(vid, pid, chip, (int)sizeof chip);
    diag_set("bt chip", "%s", chip[0] ? chip : "unknown (no device descriptor)");
    log_line("hci_usb: chip %s", chip[0] ? chip : "unknown");
    return 1;
fail:
    e = errno;
    log_line("hci_usb: %s unusable: %s", path, strerror(e));
    diag_set("bt controller", "%s %s unusable (errno %d)", path, id, e);
    /* Give back what was taken, so a retry starts from nothing. */
    for (i = 0; i < SLOT_COUNT; i++) {
        struct usb_fs_close cl;
        if (!u->sl[i].open) continue;
        cl.ep_index = (uint8_t)i;
        (void)ioctl(u->fd, USB_FS_CLOSE, &cl);
    }
    if (inited) {
        struct usb_fs_uninit un;
        memset(&un, 0, sizeof un);
        (void)ioctl(u->fd, USB_FS_UNINIT, &un);
    }
    close(u->fd);
    u->fd = -1;
    memset(u->sl, 0, sizeof u->sl);
    memset(u->fsep, 0, sizeof u->fsep);
    return (e == ENOMEM || e == EBUSY) ? -1 : 0;
}

/* A second copy started right after the first one was told to stop: the
 * old one may still hold the controller's transfer slots for a moment. */
#define HCI_BUSY_RETRY_MS 8000

int hci_usb_open(hci_t *out)
{
    static const char *const nodes[] = {
        "/dev/ugen0.2", "/dev/ugen0.3", "/dev/ugen1.2", "/dev/ugen0.1",
    };
    struct usb_hci *u;
    size_t i;
    long until = now_ms() + HCI_BUSY_RETRY_MS;
    int tries = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    u = calloc(1, sizeof *u);
    if (!u) return 0;
    u->fd = -1;
    for (;;) {
        int busy = 0;
        for (i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
            int r = try_node(u, nodes[i]);
            if (r == 1) {
                static int hooked;
                out->ctx = u;
                out->ops = &usb_ops;
                g_open = u;
                if (!hooked) { atexit(release_at_exit); hooked = 1; }
                if (tries) log_line("hci_usb: controller free after %d retries", tries);
                return 1;
            }
            if (r < 0) {
                busy = 1;   /* the Bluetooth controller is still held: wait for it, */
                break;      /* never move on to other USB devices */
            }
        }
        if (!busy || now_ms() >= until || hb_stop_requested()) break;
        if (!tries) log_line("hci_usb: controller still held (another copy closing?), retrying for %d s",
                             HCI_BUSY_RETRY_MS / 1000);
        tries++;
        usleep(500 * 1000);
    }
    free(u);
    diag_set("bt controller", "NOT FOUND: no Bluetooth controller at /dev/ugen0.2, 0.3, 1.2 or 0.1 could be used");
    return 0;
}

/* ---- diagnostics: list every ugen device ---------------------------------- */

static void survey_one(const char *path, int *count)
{
    static uint8_t cfg[1024];
    char id[128], desc[DIAG_VAL_MAX - 160], key[40];
    struct usb_gen_descriptor gd;
    int fd = open(path, O_RDONLY), n;

    if (fd < 0) fd = open(path, O_RDWR);
    snprintf(key, sizeof key, "usb %s", path);
    if (fd < 0) {
        log_line("usb: %s: open errno %d", path, errno);
        diag_set(key, "cannot open (errno %d)", errno);
        (*count)++;
        return;
    }
    device_id(fd, id, sizeof id, NULL, NULL);
    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg;
    gd.ugd_maxlen = (uint16_t)sizeof cfg;
    gd.ugd_config_index = 0xFF;                 /* the current configuration */
    n = ioctl(fd, USB_GET_FULL_DESC, &gd) == 0 ? gd.ugd_actlen : 0;
    if (n > (int)sizeof cfg) n = (int)sizeof cfg;
    if (n > 0) usbhci_describe(cfg, n, desc, sizeof desc);
    else snprintf(desc, sizeof desc, "configuration descriptor unavailable (errno %d)", errno);
    close(fd);
    log_line("usb: %s is %s: %s", path, id, desc);
    diag_set(key, "%s: %s", id, desc);
    (*count)++;
}

/* opendir/readdir/closedir were not imported by 1.0.2. Every import is
 * bound by the payload runtime before main() runs, and one that does not
 * bind ends the payload silently (no notification, no log). So these are
 * looked up here, at run time, and the survey falls back to known node
 * names when they are missing. */
typedef DIR *(*opendir_fn)(const char *);
typedef struct dirent *(*readdir_fn)(DIR *);
typedef int (*closedir_fn)(DIR *);

static intptr_t libc_sym(const char *name)
{
    static const char *const libs[] = { "libSceLibcInternal.sprx", "libc.sprx" };
    size_t i;

    for (i = 0; i < sizeof libs / sizeof libs[0]; i++) {
        uint32_t h = 0;
        intptr_t a;
        if (kernel_dynlib_handle(-1, libs[i], &h) != 0 || !h) continue;
        if ((a = kernel_dynlib_dlsym(-1, h, name))) return a;
    }
    return 0;
}

static int list_ugen(char names[][32], int max)
{
    opendir_fn od = (opendir_fn)libc_sym("opendir");
    readdir_fn rd = (readdir_fn)libc_sym("readdir");
    closedir_fn cd = (closedir_fn)libc_sym("closedir");
    struct dirent *e;
    DIR *dir;
    int n = 0;

    if (!od || !rd || !cd) {
        log_line("usb: directory listing unavailable (opendir %p readdir %p closedir %p)",
                 (void *)od, (void *)rd, (void *)cd);
        return 0;
    }
    if (!(dir = od("/dev"))) return 0;
    while ((e = rd(dir)) && n < max)
        if (!strncmp(e->d_name, "ugen", 4))
            snprintf(names[n++], 32, "/dev/%.24s", e->d_name);
    cd(dir);
    return n;
}

int hci_usb_survey(void)
{
    static const char *const fallback[] = {
        "/dev/ugen0.1", "/dev/ugen0.2", "/dev/ugen0.3", "/dev/ugen1.1", "/dev/ugen1.2",
    };
    char names[24][32];
    int n = list_ugen(names, 24), count = 0, i, j;

    if (!n) {
        log_line("usb: no ugen entries listed in /dev (errno %d); trying known names", errno);
        for (i = 0; i < (int)(sizeof fallback / sizeof fallback[0]); i++)
            snprintf(names[n++], sizeof names[0], "%s", fallback[i]);
    }
    for (i = 1; i < n; i++)                       /* sort: stable report order */
        for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[32];
            memcpy(t, names[j], sizeof t);
            memcpy(names[j], names[j - 1], sizeof t);
            memcpy(names[j - 1], t, sizeof t);
        }
    for (i = 0; i < n; i++) survey_one(names[i], &count);
    diag_set("usb devices", "%d ugen node(s)", count);
    return count;
}
