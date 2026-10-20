/* The one place HearBridge PS5's version is set. */
#ifndef HEARBRIDGE_VERSION_H
#define HEARBRIDGE_VERSION_H

#ifndef HEARBRIDGE_VERSION
#define HEARBRIDGE_VERSION "1.0.1"
#endif

/* BossQuickDock: modified version of HearBridge PS5 by X-F1REBALL-X (GPL-3.0). */
/* 0 = final build: nothing is written to /data/hearbridge/hearbridge.log.
 * Set to 1 (make ps5 CFLAGS+=-DBQD_LOG_TO_FILE=1) to debug again. */
#ifndef BQD_LOG_TO_FILE
#define BQD_LOG_TO_FILE 0
#endif
#define HB_APP_NAME   "BossQuickDock"
#define HB_APP_AUTHOR "RamalhoBoss"
#define HB_APP_CREDIT "Developed by RamalhoBoss - based on HearBridge PS5 by X-F1REBALL-X (GPL-3.0)"

#endif
