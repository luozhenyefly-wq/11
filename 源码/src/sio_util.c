/*
 * sio_util.c - 日志、参数解析、信号、读写封装
 */
#define _GNU_SOURCE
#include "sio_common.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_verbose = 0;
int g_json    = 0;

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */
static void vlog(const char *tag, const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    fprintf(stderr, "[%02d:%02d:%02d.%03ld] %s ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void sio_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog("INFO ", fmt, ap); va_end(ap);
}

void sio_log_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog("WARN ", fmt, ap); va_end(ap);
}

void sio_log_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog("ERROR", fmt, ap); va_end(ap);
}

void sio_log_verbose(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap; va_start(ap, fmt); vlog("DEBUG", fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ */
/* 人类可读字节数                                                      */
/* ------------------------------------------------------------------ */
const char *sio_human(uint64_t bytes, char *buf, size_t bufsz)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, bufsz, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, bufsz, "%.2f %s", v, unit[u]);
    return buf;
}

/* ------------------------------------------------------------------ */
/* 参数解析                                                            */
/* ------------------------------------------------------------------ */
long long sio_parse_size(const char *s)
{
    char *end = NULL;
    long long v;

    if (!s || !*s)
        return -1;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || v < 0)
        return -1;

    if (*end) {
        char c = *end;
        end++;
        if (*end && !(end[1] == '\0' && (end[0] == 'B' || end[0] == 'b')))
            return -1;   /* 只允许 K/M/G 或 KB/ KiB 这类单后缀 */
        switch (c) {
        case 'k': case 'K': v *= 1024LL; break;
        case 'm': case 'M': v *= 1024LL * 1024; break;
        case 'g': case 'G': v *= 1024LL * 1024 * 1024; break;
        case 't': case 'T': v *= 1024LL * 1024 * 1024 * 1024; break;
        default: return -1;
        }
    }
    return v;
}

long long sio_parse_delay_ms(const char *s)
{
    char *end = NULL;
    double v;

    if (!s || !*s)
        return -1;

    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || v < 0.0 || *end)
        return -1;

    return (long long)(v + 0.5);
}

/* ------------------------------------------------------------------ */
/* 信号                                                                */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_int_flag = 0;
static volatile sig_atomic_t g_int_signo = 0;

static void on_signal(int signo)
{
    g_int_flag = 1;
    if (g_int_signo == 0)
        g_int_signo = signo;
    /* 不在这里做任何不可重入的操作；由主循环安全退出 */
}

void sio_install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* 不加 SA_RESTART：让阻塞的 read/write 也能被打断 */

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int sio_interrupted(void)
{
    return g_int_flag != 0;
}

const char *sio_interrupt_signal_name(void)
{
    if (g_int_signo == SIGINT)
        return "SIGINT";
    if (g_int_signo == SIGTERM)
        return "SIGTERM";
    return "-";
}

int sio_sleep_ms_interruptible(long long delay_ms)
{
    struct timespec ts;
    long long left = delay_ms;

    while (left > 0) {
        if (sio_interrupted())
            return -1;
        long long step = left > 20 ? 20 : left;
        ts.tv_sec  = step / 1000;
        ts.tv_nsec = (step % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        left -= step;
    }
    return sio_interrupted() ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* 读写封装                                                            */
/* ------------------------------------------------------------------ */
ssize_t sio_pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const char *p = (const char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, (off_t)(off + (off_t)done));
        if (n < 0) {
            if (errno == EINTR) {
                if (sio_interrupted())
                    return (ssize_t)done;
                continue;
            }
            return -1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t sio_pread_full(int fd, void *buf, size_t len, off_t off)
{
    char *p = (char *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, (off_t)(off + (off_t)done));
        if (n < 0) {
            if (errno == EINTR) {
                if (sio_interrupted())
                    return (ssize_t)done;
                continue;
            }
            return -1;
        }
        if (n == 0)
            break;      /* EOF：空洞区读出来就是 0 */
        done += (size_t)n;
    }
    return (ssize_t)done;
}
