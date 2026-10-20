/* usbopen_test.c - isolation test for the DualSense reconnect bug.
 *
 * Opens the console's Bluetooth controller the same way the app does
 * (USB_FS_INIT + 62 FS_OPEN slots on the event / ACL pipes), but never
 * starts a transfer and never sends an HCI command. Waits 20 s, closes
 * everything and exits. If the pad cannot reconnect afterwards, merely
 * opening the pipes breaks the system's Bluetooth stack.
 *
 * Log: /data/usbopen_test.log */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

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

    g_log = fopen("/data/usbopen_test.log", "w");
    logf("usbopen_test: open only, no transfer, no HCI command");

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
    logf("%d of %d slots opened on %s; waiting 20 s", opened, N_SLOTS, NODE);

    sleep(20);

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
