/* usbdrain_test.c - isolation test 3 for the DualSense reconnect bug.
 *
 * Arms 8 event reads (no ACL read) for 3 s, then NEVER cancels them:
 * sends harmless HCI Read Local Version commands until our own answers
 * have completed every pending read, then closes with nothing pending.
 * If the pad still reconnects afterwards, cancelling reads was the bug.
 *
 * Log: /data/usbdrain_test.log */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#define NODE      "/dev/ugen0.2"
#define EVT_EP    0x82
#define ACLIN_EP  0x81
#define ACLOUT_EP 0x01
#define SPARE_EP  0x02
#define N_EVT     40
#define N_ACL     20
#define N_SLOTS   (N_EVT + N_ACL + 2)
#define BUFSZ     1024

static FILE *g_log;

static void logf(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!g_log) return;
    fprintf(g_log, "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static struct usb_fs_endpoint g_ep[N_SLOTS];

static unsigned char g_buf[N_EVT + N_ACL][BUFSZ];
static void *g_bufp[N_EVT + N_ACL];
static uint32_t g_len[N_EVT + N_ACL];

static void arm(int fd, int idx)
{
    struct usb_fs_start st;
    g_ep[idx].nFrames = 1;
    g_ep[idx].aFrames = 0;
    g_ep[idx].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    g_ep[idx].timeout = USB_FS_TIMEOUT_NONE;
    g_ep[idx].status = 0;
    g_len[idx] = BUFSZ;
    st.ep_index = (uint8_t)idx;
    if (ioctl(fd, USB_FS_START, &st) < 0) logf("FS_START slot %d: %s", idx, strerror(errno));
}

#define ARMED 8
static int g_busy[N_EVT + N_ACL];

/* HCI command over the control pipe (class request to interface 0). */
static int hci_cmd(int fd, unsigned op)
{
    unsigned char pkt[3];
    struct usb_ctl_request rq;
    unsigned char *setup;
    pkt[0] = (unsigned char)(op & 0xFF);
    pkt[1] = (unsigned char)(op >> 8);
    pkt[2] = 0;
    memset(&rq, 0, sizeof rq);
    setup = (unsigned char *)&rq.ucr_request;
    setup[0] = 0x20;                 /* class, host to device */
    setup[6] = 3;                    /* wLength */
    rq.ucr_data = pkt;
    if (ioctl(fd, USB_DO_REQUEST, &rq) < 0) {
        logf("HCI command 0x%04x: %s", op, strerror(errno));
        return 0;
    }
    return 1;
}

static int open_slot(int fd, int idx, uint8_t addr)
{
    struct usb_fs_open op;
    memset(&op, 0, sizeof op);
    op.ep_index = (uint8_t)idx;
    op.ep_no = addr;
    op.max_bufsize = BUFSZ;
    op.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &op) < 0) {
        logf("FS_OPEN slot %d ep 0x%02x: %s", idx, addr, strerror(errno));
        return 0;
    }
    return 1;
}

int main(void)
{
    struct usb_fs_init in;
    struct usb_fs_uninit un;
    int fd, i, opened = 0;

    g_log = fopen("/data/usbdrain_test.log", "w");
    logf("usbdrain_test: 8 event reads, drained by our own commands, never cancelled");

    fd = open(NODE, O_RDWR);
    if (fd < 0) {
        logf("open %s: %s", NODE, strerror(errno));
        return 1;
    }
    memset(&in, 0, sizeof in);
    in.pEndpoints = g_ep;
    in.ep_index_max = N_SLOTS;
    if (ioctl(fd, USB_FS_INIT, &in) < 0) {
        logf("FS_INIT: %s", strerror(errno));
        close(fd);
        return 1;
    }
    for (i = 0; i < N_EVT; i++) opened += open_slot(fd, i, EVT_EP);
    for (i = 0; i < N_ACL; i++) opened += open_slot(fd, N_EVT + i, ACLIN_EP);
    opened += open_slot(fd, N_EVT + N_ACL, ACLOUT_EP);
    opened += open_slot(fd, N_EVT + N_ACL + 1, SPARE_EP);
    logf("%d of %d slots opened on %s", opened, N_SLOTS, NODE);

    /* arm 8 event reads only */
    for (i = 0; i < N_EVT + N_ACL; i++) {
        g_bufp[i] = g_buf[i];
        g_ep[i].ppBuffer = &g_bufp[i];
        g_ep[i].pLength = &g_len[i];
        g_ep[i].nFrames = 1;
    }
    for (i = 0; i < ARMED; i++) { arm(fd, i); g_busy[i] = 1; }
    {
        struct timespec t0, t;
        int pending = ARMED, sent = 0, ours = 0, other = 0;
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            struct usb_fs_complete c;
            struct pollfd pf;
            clock_gettime(CLOCK_MONOTONIC, &t);
            ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
            for (;;) {
                int idx;
                memset(&c, 0, sizeof c);
                if (ioctl(fd, USB_FS_COMPLETE, &c) < 0) break;
                idx = c.ep_index;
                if (idx >= ARMED || !g_busy[idx]) continue;
                g_busy[idx] = 0;
                pending--;
                if (g_ep[idx].status == 0) {
                    unsigned char *b = g_buf[idx];
                    unsigned n = g_len[idx];
                    if (b[0] == 0x0E && n >= 5 && (b[3] | (b[4] << 8)) == 0x1001) ours++;
                    else {
                        other++;
                        if (b[0] == 0x0E && n >= 5)
                            logf("TOOK system event: Command Complete op 0x%04x", b[3] | (b[4] << 8));
                        else if (b[0] == 0x0F && n >= 6)
                            logf("TOOK system event: Command Status op 0x%04x", b[4] | (b[5] << 8));
                        else
                            logf("TOOK system event 0x%02x len %u", b[0], n);
                    }
                } else {
                    logf("slot %d finished with status %d", idx, g_ep[idx].status);
                }
                /* never re-armed: we are draining */
            }
            if (!pending) { logf("all %d reads drained at %ld ms: %d by our answers, %d system events, %d commands sent", ARMED, ms, ours, other, sent); break; }
            if (ms >= 3000) {
                if (ms > 15000) { logf("NOT drained after 15 s (%d pending) - closing anyway", pending); break; }
                if (hci_cmd(fd, 0x1001)) sent++;
            }
            pf.fd = fd; pf.events = POLLIN | POLLRDNORM; pf.revents = 0;
            (void)poll(&pf, 1, ms >= 3000 ? 20 : 50);
        }
    }

    for (i = 0; i < N_SLOTS; i++) {
        struct usb_fs_close cl;
        cl.ep_index = (uint8_t)i;
        (void)ioctl(fd, USB_FS_CLOSE, &cl);
    }
    memset(&un, 0, sizeof un);
    (void)ioctl(fd, USB_FS_UNINIT, &un);
    close(fd);
    logf("closed; done");
    if (g_log) fclose(g_log);
    return 0;
}
