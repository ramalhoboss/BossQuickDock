/* hci_usb.h - HCI over the console's internal USB Bluetooth controller.
 *
 * Implements the hci_ops contract from hci.h on top of a FreeBSD ugen
 * node, following the Bluetooth Core spec Vol 4 Part B (USB transport):
 *   commands  -> class request on the default control pipe (bmRequestType 0x20)
 *   events    <- interrupt IN pipe
 *   ACL data <-> bulk IN / bulk OUT pipes */
#ifndef HEARBRIDGE_HCI_USB_H
#define HEARBRIDGE_HCI_USB_H

#include "hci.h"

/* Find and open the controller, filling *out on success.
 * Returns 1 on success, 0 on failure (out is left zeroed). */
int hci_usb_open(hci_t *out);

/* USB VID:PID of the opened controller (see btchip.h for the vendor).
 * Returns 1 once hci_usb_open() succeeded, else 0 with both set to -1. */
int hci_usb_chip(int *vid, int *pid);

/* Called (outside the USB completion loop) for every HCI Connection Request
 * event the controller sends, and once per pump with ev NULL. Set by main. */
extern void (*hci_usb_conn_req_hook)(hci_t hci, const unsigned char *ev, int n);

/* #34: stop reading the shared event / ACL pipes for ms, so the system
 * stack gets every packet of a device that is not ours (a pad calling in). */
void hci_usb_yield(int ms);

/* How long we stay off the pipes after a device that is not ours calls in
 * or drops: long enough for a pad's page, pairing and encryption setup. */
#define HB_YIELD_IDLE_MS   45000
#define HB_YIELD_STREAM_MS 45000
/* A link we never saw come up closes while our own page is out: most likely
 * ours, its Connection Complete read by the system, which hung up on it.
 * Off only until the system has its answer (replay), then we page again. */
#define HB_YIELD_PAGE_MS   2000

/* Our own link's address (NULL when we have none): a Disconnection
 * Complete for any other handle makes us step aside (hci_usb_yield). */
void hci_usb_own(const unsigned char addr[6]);

/* #34: what our own link needs now, so only that many reads stay armed on
 * the pipes we share with the system (see hci_usb.c, share mode). */
void hci_usb_hint(int streaming, int connecting, int sticky);

/* Diagnostics: open every /dev/ugen* node read-only, log and report (diag.h)
 * its VID:PID, names, interfaces (class/subclass/protocol) and endpoints,
 * then close it again. Nothing is sent to any device. Call before
 * hci_usb_open(). Returns the number of nodes listed. */
int hci_usb_survey(void);

#endif
