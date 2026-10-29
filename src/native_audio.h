/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BQD_NATIVE_AUDIO_H
#define BQD_NATIVE_AUDIO_H
int native_audio_start(int format,int seconds);
int native_audio_live(const char *ip,int port);
void native_audio_cancel(void);
void native_audio_shutdown(void);
int native_audio_busy(void);
int native_audio_json(char *out,int max);
/* Returns an open descriptor for the latest completed export, or -1. */
int native_audio_download(int raw,char *name,int max);
#endif
