/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "capture_guard.h"
#include <pthread.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int current;
int capture_guard_take(int owner){int ok;pthread_mutex_lock(&lock);ok=current==0;if(ok)current=owner;pthread_mutex_unlock(&lock);return ok;}
void capture_guard_release(int owner){pthread_mutex_lock(&lock);if(current==owner)current=0;pthread_mutex_unlock(&lock);}
int capture_guard_owner(void){int v;pthread_mutex_lock(&lock);v=current;pthread_mutex_unlock(&lock);return v;}
