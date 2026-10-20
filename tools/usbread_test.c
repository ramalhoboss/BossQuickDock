/* usbread_test.c - isolation test 2 for the DualSense reconnect bug.
 *
 * Opens the controller like the app, keeps all 40 event / 20 ACL reads
 * armed for 8 s (the app's start-up window) and logs every packet taken,
 * then stops the reads, puts both pipes' data toggle back in step and
 * closes. No HCI command is ever sent.
 *
 * Log: /data/usbread_test.log */
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

static void resync(int fd, int idx, uint8_t addr)
{
    struct usb_fs_clear_stall_sync cs;
    int tries;
    cs.ep_index = (uint8_t)idx;
    for (tries = 0; tries < 100; tries++) {
        if (ioctl(fd, USB_FS_CLEAR_STALL_SYNC, &cs) == 0) {
            logf("pipe 0x%02x toggle reset ok (try %d)", addr, tries + 1);
            return;
        }
        if (errno != EBUSY) break;
        usleep(5000);
    }
    logf("pipe 0x%02x toggle reset failed: %s", addr, strerror(errno));
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

    g_log = fopen("/data/usbread_test.log", "w");
    logf("usbread_test: reads armed 8 s, no HCI command");

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

    /* arm every read */
    for (i = 0; i < N_EVT + N_ACL; i++) {
        g_bufp[i] = g_buf[i];
        g_ep[i].ppBuffer = &g_bufp[i];
        g_ep[i].pLength = &g_len[i];
        g_ep[i].nFrames = 1;
        arm(fd, i);
    }
    {
        struct timespec t0, t;
        int nev = 0, nacl = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            struct usb_fs_complete c;
            struct pollfd pf;
            clock_gettime(CLOCK_MONOTONIC, &t);
            if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= 8000) break;
            for (;;) {
                int idx;
                memset(&c, 0, sizeof c);
                if (ioctl(fd, USB_FS_COMPLETE, &c) < 0) break;
                idx = c.ep_index;
                if (idx >= N_EVT + N_ACL) continue;
                if (g_ep[idx].status == 0) {
                    unsigned char *b = g_buf[idx];
                    unsigned n = g_len[idx];
                    if (idx < N_EVT) {
                        nev++;
                        if (b[0] == 0x0E && n >= 5)
                            logf("TOOK event 0x0E Command Complete op 0x%04x status 0x%02x", b[3] | (b[4] << 8), n > 5 ? b[5] : 0);
                        else if (b[0] == 0x0F && n >= 6)
                            logf("TOOK event 0x0F Command Status op 0x%04x status 0x%02x", b[4] | (b[5] << 8), b[2]);
                        else
                            logf("TOOK event 0x%02x len %u", b[0], n);
                    } else {
                        nacl++;
                        if (nacl <= 30 || nacl % 100 == 0)
                            logf("TOOK ACL handle 0x%03x len %u (#%d)", (b[0] | (b[1] << 8)) & 0x0FFF, n, nacl);
                    }
                }
                arm(fd, idx);
            }
            pf.fd = fd; pf.events = POLLIN | POLLRDNORM; pf.revents = 0;
            (void)poll(&pf, 1, 50);
        }
        logf("8 s done: %d events, %d ACL frames taken", nev, nacl);
    }
    /* stop the reads, wait for them to finish, then resync both pipes */
    for (i = 0; i < N_EVT + N_ACL; i++) {
        struct usb_fs_stop sp;
        sp.ep_index = (uint8_t)i;
        (void)ioctl(fd, USB_FS_STOP, &sp);
    }
    usleep(100000);
    {
        struct usb_fs_complete c;
        while (ioctl(fd, USB_FS_COMPLETE, &c) == 0) { }
    }
    resync(fd, 0, EVT_EP);
    resync(fd, N_EVT, ACLIN_EP);

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
