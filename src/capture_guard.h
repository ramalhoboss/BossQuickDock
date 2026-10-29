/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BQD_CAPTURE_GUARD_H
#define BQD_CAPTURE_GUARD_H
int capture_guard_take(int owner);
void capture_guard_release(int owner);
int capture_guard_owner(void);
#endif
