/* log.c - timestamped log lines, flushed immediately so nothing is lost if
 * the payload is killed. */
#include "log.h"
#include "util.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE *log_fp;
static char log_path[256];

/* Size cap: past LOG_MAX the file becomes <path>.old (one generation) and a
 * fresh one starts, so the log can no longer fill the disk. */
#define LOG_MAX (5L * 1024 * 1024)
static long log_size;
static volatile int file_lock __attribute__((unused));

static void log_rotate(void)
{
    char old[sizeof log_path + 8];
    if (!log_fp || !log_path[0]) return;
    fclose(log_fp);
    snprintf(old, sizeof old, "%s.old", log_path);
    (void)remove(old);
    (void)rename(log_path, old);
    log_fp = fopen(log_path, "w");
    log_size = 0;
}

/* Last lines kept in memory so /api/diag can show them without file access. */
#define TAIL_CAP 6144
static char tail_buf[TAIL_CAP];
static unsigned tail_pos;            /* total bytes ever written */
static volatile int tail_lock;

static void tail_add(const char *s, size_t n)
{
    size_t i;
    while (__sync_lock_test_and_set(&tail_lock, 1)) { }
    for (i = 0; i < n; i++) tail_buf[(tail_pos + i) % TAIL_CAP] = s[i];
    tail_pos += (unsigned)n;
    __sync_lock_release(&tail_lock);
}

int log_tail(char *out, size_t cap)
{
    unsigned have, start, i, n;
    if (!out || !cap) return 0;
    while (__sync_lock_test_and_set(&tail_lock, 1)) { }
    have = tail_pos < TAIL_CAP ? tail_pos : TAIL_CAP;
    start = tail_pos - have;
    n = have < cap - 1 ? have : (unsigned)(cap - 1);
    start += have - n;
    for (i = 0; i < n; i++) out[i] = tail_buf[(start + i) % TAIL_CAP];
    out[n] = 0;
    __sync_lock_release(&tail_lock);
    /* start at a line boundary */
    if (have > n || tail_pos > TAIL_CAP) {
        char *nl = strchr(out, '\n');
        if (nl) { memmove(out, nl + 1, strlen(nl + 1) + 1); n = (unsigned)strlen(out); }
    }
    return (int)n;
}

int log_open(const char *path)
{
    FILE *f;

    if (!path)
        return 0;
#if !BQD_LOG_TO_FILE
    return 1;   /* final build: no log file on the console (the in-memory tail for /api/diag stays) */
#endif
    f = fopen(path, "a");
    if (!f)
        return 0;
    log_close();
    log_fp = f;
    snprintf(log_path, sizeof log_path, "%s", path);
    {
        struct stat st;
        log_size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    }
    if (log_size > LOG_MAX) log_rotate();   /* an old oversized log goes at start */
    return 1;
}

void log_close(void)
{
    if (log_fp)
        fclose(log_fp);
    log_fp = NULL;
}

void log_line(const char *fmt, ...)
{
    FILE *dst;
    long t = now_ms();
    char line[640];
    int w;
    va_list args;

    w = snprintf(line, sizeof line, "[%ld.%03ld] ", t / 1000, t % 1000);
    if (w < 0) w = 0;
    va_start(args, fmt);
    (void)vsnprintf(line + w, sizeof line - (size_t)w - 1, fmt, args);
    va_end(args);
    w = (int)strlen(line);
    line[w++] = '\n';
    line[w] = 0;
#if BQD_LOG_TO_FILE
    while (__sync_lock_test_and_set(&file_lock, 1)) { }
    dst = log_fp ? log_fp : stderr;
    fputs(line, dst);
    fflush(dst);
    if (dst == log_fp && (log_size += w) > LOG_MAX) log_rotate();
    __sync_lock_release(&file_lock);
#else
    (void)dst;
#endif
    tail_add(line, (size_t)w);
}
