/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BQD_OPUS_WIRE_H
#define BQD_OPUS_WIRE_H
#include <stddef.h>
#include <stdint.h>
int opus_wire_ipv4(const char *text,unsigned char out[4]);
int opus_wire_packet(unsigned char *out,size_t capacity,uint64_t session,uint32_t seq,uint64_t pts,uint64_t elapsed,const void *payload,size_t bytes);
#endif
