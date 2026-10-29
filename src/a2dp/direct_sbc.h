/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BQD_DIRECT_SBC_H
#define BQD_DIRECT_SBC_H
/* Direct SBC (1.6.0-direct10): PCM-clocked sending, 4 frames per packet and
 * a 3-packet queue whose packets expire after 24 ms. On the console (Sonic
 * Sub 630, 2026-10-28) the controller's 60-90 ms credit gaps expired about
 * 10 packets a second (1139 of 4220 dropped in 131 s, bitpool down to 22),
 * so it is off and SBC takes the paced path again. -DBQD_DIRECT_SBC=1 to try. */
#ifndef BQD_DIRECT_SBC
#define BQD_DIRECT_SBC 0
#endif
#define BQD_SBC_PACKET_FRAMES 4
#define BQD_SBC_QUEUE_PKTS 3
#define BQD_SBC_MAX_OUTSTANDING 3
#define BQD_SBC_MAX_AGE_MS 24
#define BQD_SBC_CAPTURE_GAP_MS 50
static inline int bqd_sbc_expired(long queued,long now){return now>=queued && now-queued>=BQD_SBC_MAX_AGE_MS;}
#endif
