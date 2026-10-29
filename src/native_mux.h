/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BQD_NATIVE_MUX_H
#define BQD_NATIVE_MUX_H
/* Converts the bounded probe container into ADTS AAC or Ogg Opus.
 * Opus pre-skip is unknown; zero preserves all captured decoded samples. */
int native_mux(const char *input,const char *output,int format);
#endif
