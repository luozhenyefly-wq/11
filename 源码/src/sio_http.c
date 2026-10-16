/*
 * sio_http.c - 数据源：file:// 本地文件 或 http(s):// Range 请求
 *
 * 作业场景是「下载器」，所以真正的数据源是 HTTP Range 请求：
 *   GET <path> HTTP/1.1
 *   Range: bytes=<off>-<off+len-1>
 * 服务端回 206 Partial Content，只把要的那一段发过来 —— 正好对上
 * 「Iseek + write 定点写入」的语义：要哪一块就取哪一块，其余空洞不动。
 *
 * 同时提供 file:// 源，用于无网络环境下的自动化测试与结果复现。
 */
#define _GNU_SOURCE
#include "sio_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct sio_source {
    sio_source_info info;
    int      fd;          /* 本地文件源 */
    int      sock;        /* HTTP 源当前连接，-1 表示未连接 */
    int      sock_dirty;  /* 1 = 本连接上曾走过 200 降级路径，流里有未读残留，
                           *     读完本块响应体后必须关闭，下一块重新建连 */
    int      use_curl;    /* 1 = HTTP 源改用 curl 拉取 Range 段（默认，更快更稳）；
                           *     0 = 用内置 socket 实现（curl 不可用时的回退） */
    char     host[256];
    uint16_t port;
    char     path[SIO_URL_MAX];
};

/* ------------------------------------------------------------------ */
/* HTTP 段拉取：优先使用 curl -r a-b（与 Range 语义一一对应）           */
/* ------------------------------------------------------------------ */
static int have_curl(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (system("command -v curl >/dev/null 2>&1") == 0);
    return cached;
}

/* 用 curl 把 [off, off+len) 拉进 buf；返回实际字节数，<0 出错。
 * curl 的 -r a-b 正好生成 `Range: bytes=a-b`，206 时把该段写进 stdout。 */
static ssize_t curl_pread(const char *url, void *buf, size_t len, uint64_t off)
{
    char cmd[SIO_URL_MAX + 256];
    FILE *fp;
    size_t got = 0, want = len;

    /* --fail: 服务端返回 4xx/5xx 时 curl 退出码非 0，这里才能识别成错误 */
    int rc = snprintf(cmd, sizeof(cmd),
                      "curl -sS --fail --max-time 120 -r %llu-%llu '%s'",
                      (unsigned long long)off, (unsigned long long)(off + len - 1),
                      url);
    if (rc <= 0 || (size_t)rc >= sizeof(cmd))
        return -1;

    fp = popen(cmd, "r");
    if (!fp)
        return -1;

    while (got < want) {
        size_t n = fread((char *)buf + got, 1, want - got, fp);
        if (n == 0)
            break;
        got += n;
    }
    /* 必须读完并检查退出码：curl 出错时 pclose 返回非 0 */
    {
        char sink[4096];
        while (fread(sink, 1, sizeof(sink), fp) > 0)
            ;   /* 丢弃多余字节（正常不会发生） */
    }
    int st = pclose(fp);
    if (st == 0 && got == want)
        return (ssize_t)got;
    if (got > 0 && st != 0)
        return -1;          /* 传输中途出错，不能把半段当成功 */
    return (ssize_t)got;
}

/* curl 探测源长度：-r 0-0 + -D - 拿 Content-Range 里的总长 */
static int curl_probe_size(sio_source *s)
{
    char cmd[SIO_URL_MAX + 256];
    char line[512];
    FILE *fp;

    int rc = snprintf(cmd, sizeof(cmd),
                      "curl -sS --fail --max-time 60 -r 0-0 -D - -o /dev/null '%s'",
                      s->info.uri);
    if (rc <= 0 || (size_t)rc >= sizeof(cmd))
        return SIO_ERR_NET;

    fp = popen(cmd, "r");
    if (!fp)
        return SIO_ERR_NET;

    while (fgets(line, sizeof(line), fp)) {
        if (strncasecmp(line, "Content-Range:", 14) == 0) {
            unsigned long long a = 0, b = 0, t = 0;
            if (sscanf(line + 14, " bytes %llu-%llu/%llu", &a, &b, &t) == 3) {
                s->info.size = t;
                pclose(fp);
                return SIO_OK;      /* 日志由调用方统一打印，避免重复 */
            }
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            unsigned long long cl = strtoull(line + 15, NULL, 10);
            if (cl > 0 && s->info.size == 0)
                s->info.size = cl;      /* 服务端不支持 Range 时的兜底 */
        }
    }
    pclose(fp);
    if (s->info.size == 0)
        sio_log_warn("无法从响应头得到源长度，需要显式指定 --size");
    return SIO_OK;
}

/* ------------------------------------------------------------------ */
/* URL 解析                                                            */
/* ------------------------------------------------------------------ */
static int split_url(const char *url, char *scheme, size_t scheme_sz,
                     char *host, size_t host_sz, uint16_t *port,
                     char *path, size_t path_sz)
{
    const char *p = strstr(url, "://");
    const char *rest;
    const char *slash;

    if (!p)
        return -1;

    snprintf(scheme, scheme_sz, "%.*s", (int)(p - url), url);
    rest = p + 3;
    slash = strchr(rest, '/');
    if (!slash) {
        snprintf(host, host_sz, "%s", rest);
        snprintf(path, path_sz, "/");
    } else {
        size_t hlen = (size_t)(slash - rest);
        snprintf(host, host_sz, "%.*s", (int)(hlen > host_sz - 1 ? host_sz - 1 : hlen), rest);
        snprintf(path, path_sz, "%s", slash);
    }

    *port = (strcmp(scheme, "https") == 0) ? 443 : 80;

    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        long p2 = strtol(colon + 1, NULL, 10);
        if (p2 > 0 && p2 < 65536)
            *port = (uint16_t)p2;
        else
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 带超时的 TCP 连接                                                    */
/* ------------------------------------------------------------------ */
static int tcp_connect(const char *host, uint16_t port, int timeout_sec)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int rc, sock = -1;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        sio_log_err("域名解析失败 %s: %s", host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;

        struct timeval tv = { timeout_sec, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    if (sock < 0)
        sio_log_err("连接 %s:%u 失败: %s", host, port, strerror(errno));
    return sock;
}

/* 读满 n 字节（HTTP 响应体必须精确）；返回实际读取 */
static ssize_t sock_read_full(int sock, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = recv(sock, p + done, n - done, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return done > 0 ? (ssize_t)done : -1;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* 逐字节读一行（到 \n），用于解析响应头 */
static ssize_t sock_read_line(int sock, char *buf, size_t cap)
{
    size_t i = 0;

    while (i + 1 < cap) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        if (c == '\n') {
            buf[i] = '\0';
            if (i > 0 && buf[i - 1] == '\r')
                buf[i - 1] = '\0';
            return (ssize_t)i;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

static int http_request_range(sio_source *s, uint64_t off, size_t len,
                              uint64_t *body_off)
{
    char req[1024];
    char line[2048];
    uint64_t start = off;
    /* 缺陷-6（AI 代码审查）：end 原先按 (off + len - 1) 计算，len == 0 时会下溢成
     * 一个巨大的值，拼出 "Range: bytes=X-18446744073709551615" 这种非法请求头。
     * 这里对 len == 0 单独兜底成 start，保证请求头永远合法。 */
    uint64_t end = (len == 0) ? off : (off + (uint64_t)len - 1);
    int status = 0;
    ssize_t n;
    int got_cr = 0, have_clen = 0;
    uint64_t clen = 0;

    if (s->sock < 0) {
        s->sock = tcp_connect(s->host, s->port, 10);
        if (s->sock < 0)
            return SIO_ERR_NET;
        s->sock_dirty = 0;      /* 新连接，流是干净的 */
    }

    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "User-Agent: sparseio/%s\r\n"
                      "Accept: */*\r\n"
                      "Connection: keep-alive\r\n"
                      "Range: bytes=%llu-%llu\r\n"
                      "\r\n",
                      s->path, s->host, (unsigned)s->port, SIO_VERSION,
                      (unsigned long long)start, (unsigned long long)end);
    if (rl <= 0 || (size_t)rl >= sizeof(req))
        return SIO_ERR_USAGE;

    if (send(s->sock, req, (size_t)rl, 0) != (ssize_t)rl) {
        sio_log_warn("Range 请求发送失败，重连一次: %s", strerror(errno));
        close(s->sock);
        s->sock = -1;
        s->sock = tcp_connect(s->host, s->port, 10);
        if (s->sock < 0)
            return SIO_ERR_NET;
        s->sock_dirty = 0;
        if (send(s->sock, req, (size_t)rl, 0) != (ssize_t)rl)
            return SIO_ERR_NET;
    }

    /* 状态行 */
    n = sock_read_line(s->sock, line, sizeof(line));
    if (n <= 0) {
        sio_log_err("读取 HTTP 状态行失败");
        return SIO_ERR_NET;
    }
    if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
        sio_log_err("无法解析状态行: %s", line);
        return SIO_ERR_NET;
    }

    /* 头部 */
    for (;;) {
        n = sock_read_line(s->sock, line, sizeof(line));
        if (n < 0)
            return SIO_ERR_NET;
        if (n == 0)
            break;                     /* 空行：头部结束 */

        if (strncasecmp(line, "Content-Range:", 14) == 0 && status == 206) {
            unsigned long long a = 0, b = 0, t = 0;
            if (sscanf(line + 14, " bytes %llu-%llu/%llu", &a, &b, &t) == 3) {
                if (a == start) {
                    got_cr = 1;
                } else {
                    sio_log_err("服务端返回的区间不符合要求：请求 %llu-，返回 %llu-",
                                (unsigned long long)start, a);
                    return SIO_ERR_RANGE;
                }
            }
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            clen = strtoull(line + 15, NULL, 10);
            have_clen = 1;
        }
    }

    if (status == 206) {
        if (!got_cr) {
            sio_log_err("206 响应缺少 Content-Range");
            return SIO_ERR_RANGE;
        }
        *body_off = start;
        return SIO_OK;
    }

    if (status == 200) {
        /*
         * 服务端忽略了 Range（把整个文件发过来）。降级策略：在流里跳过前 start 字节。
         *
         * 缺陷-12（AI 代码审查，实测复现）：
         * 这里的“跳过”只是客户端不再读那一段，服务端仍然会把剩余部分写进同一个
         * socket。于是连接里残留着上一次响应没读完的字节，下一个块的请求就会把这些
         * 残留当成 HTTP 状态行来解析 —— 表现为 “无法解析状态行: <乱码>”、大量重连。
         * 修复：走 200 降级路径时明确关闭连接，让 next request 重新建连；
         * HTTP/1.1 的 206 分支由 Content-Length 精确分隔，仍可安全复用长连接。
         */
        sio_log_verbose("服务端不支持 Range（返回 200），按整文件流式跳过 %llu 字节",
                        (unsigned long long)start);
        if (have_clen && clen < start + (uint64_t)len) {
            sio_log_err("服务端未实现 Range 且响应体只有 %llu 字节，不足以覆盖请求区间 "
                        "[%llu,+%zu)",
                        (unsigned long long)clen, (unsigned long long)start, len);
            close(s->sock);
            s->sock = -1;
            return SIO_ERR_RANGE;
        }
        uint64_t skip = start;
        char junk[65536];
        while (skip > 0) {
            size_t take = skip > sizeof(junk) ? sizeof(junk) : (size_t)skip;
            ssize_t r = sock_read_full(s->sock, junk, take);
            if (r <= 0) {
                close(s->sock);
                s->sock = -1;
                return SIO_ERR_NET;
            }
            skip -= (uint64_t)r;
        }
        *body_off = start;
        /* 不能在这里 close：调用方马上还要从这个 socket 读响应体。
         * 只打标记，等响应体读完后再由 sio_source_pread 关闭（缺陷-12 修复）。 */
        s->sock_dirty = 1;
        sio_log_verbose("服务端不支持 Range：本块读完后将关闭连接，下一块重新建连");
        return SIO_OK;
    }

    sio_log_err("HTTP 状态 %d 不可用（需要 206 Partial Content）", status);
    if (status == 416)
        return SIO_ERR_RANGE;
    return SIO_ERR_NET;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
int sio_source_open(sio_source **out, const char *url)
{
    sio_source *s = calloc(1, sizeof(*s));
    if (!s)
        return SIO_ERR_IO;

    s->fd   = -1;
    s->sock = -1;

    if (split_url(url, s->info.scheme, sizeof(s->info.scheme),
                  s->host, sizeof(s->host), &s->port,
                  s->path, sizeof(s->path)) != 0) {
        sio_log_err("URL 格式不支持: %s（需 file:// 或 http://）", url);
        free(s);
        return SIO_ERR_USAGE;
    }
    snprintf(s->info.uri, sizeof(s->info.uri), "%s", url);

    if (strcmp(s->info.scheme, "file") == 0) {
        s->info.is_http = 0;
        s->fd = open(s->path, O_RDONLY);
        if (s->fd < 0) {
            sio_log_err("打开源文件 %s 失败: %s", s->path, strerror(errno));
            free(s);
            return SIO_ERR_IO;
        }
        struct stat sb;
        if (fstat(s->fd, &sb) == 0)
            s->info.size = (uint64_t)sb.st_size;
        sio_log_verbose("数据源就绪: file %s (%llu 字节)", s->path,
                        (unsigned long long)s->info.size);
        *out = s;
        return SIO_OK;
    }

    if (strcmp(s->info.scheme, "http") == 0 || strcmp(s->info.scheme, "https") == 0) {
        s->info.is_http = 1;
        if (have_curl()) {
            /* 默认走 curl：Range 语义由 curl 保证，不依赖自己维护 HTTP 长连接 */
            s->use_curl = 1;
            sio_log_verbose("数据源就绪: %s %s:%u%s（curl 通道）", s->info.scheme,
                            s->host, (unsigned)s->port, s->path);
            *out = s;
            return SIO_OK;
        }
        s->use_curl = 0;
        s->sock = tcp_connect(s->host, s->port, 10);
        if (s->sock < 0) {
            free(s);
            return SIO_ERR_NET;
        }
        sio_log_warn("未找到 curl，回退到内置 socket 实现");
        sio_log_verbose("数据源就绪: %s %s:%u%s", s->info.scheme, s->host,
                        (unsigned)s->port, s->path);
        *out = s;
        return SIO_OK;
    }

    sio_log_err("不支持的协议: %s", s->info.scheme);
    free(s);
    return SIO_ERR_USAGE;
}

/* 用一个 Range: bytes=0-0 请求探测源总长度（顺带验证服务端支持 Range） */
int sio_source_probe_size(sio_source *s)
{
    char req[1024];
    char line[2048];
    int status = 0;
    ssize_t n;
    uint64_t total = 0;
    int supports_range = 0;

    if (!s->info.is_http)
        return SIO_OK;

    if (s->use_curl)
        return curl_probe_size(s);

    if (s->sock < 0) {
        s->sock = tcp_connect(s->host, s->port, 10);
        if (s->sock < 0)
            return SIO_ERR_NET;
    }

    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "User-Agent: sparseio/%s\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n"
                      "Range: bytes=0-0\r\n"
                      "\r\n",
                      s->path, s->host, (unsigned)s->port, SIO_VERSION);
    if (rl <= 0 || (size_t)rl >= sizeof(req))
        return SIO_ERR_USAGE;
    if (send(s->sock, req, (size_t)rl, 0) != (ssize_t)rl)
        return SIO_ERR_NET;

    n = sock_read_line(s->sock, line, sizeof(line));
    if (n <= 0 || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1)
        return SIO_ERR_NET;

    for (;;) {
        n = sock_read_line(s->sock, line, sizeof(line));
        if (n <= 0)
            break;
        if (strncasecmp(line, "Content-Range:", 14) == 0) {
            unsigned long long a = 0, b = 0, t = 0;
            if (sscanf(line + 14, " bytes %llu-%llu/%llu", &a, &b, &t) == 3) {
                total = t;
                supports_range = 1;
            }
        } else if (strncasecmp(line, "Content-Length:", 15) == 0 && status == 200) {
            total = strtoull(line + 15, NULL, 10);
        }
    }

    close(s->sock);
    s->sock = -1;

    if (total) {
        s->info.size = total;
        sio_log_info("源长度 : %llu 字节%s", (unsigned long long)total,
                     supports_range ? "（服务端支持 Range 断点请求）"
                                    : "（服务端不支持 Range，将整文件流式跳过）");
    } else {
        sio_log_warn("无法从响应头得到源长度，需要显式指定 --size");
    }
    return SIO_OK;
}

void sio_source_close(sio_source *s)
{
    if (!s)
        return;
    if (s->fd >= 0)
        close(s->fd);
    if (s->sock >= 0)
        close(s->sock);
    free(s);
}

const sio_source_info *sio_source_get_info(const sio_source *s)
{
    return &s->info;
}

ssize_t sio_source_pread(sio_source *s, void *buf, size_t len, uint64_t off)
{
    if (!s->info.is_http) {
        ssize_t n = sio_pread_full(s->fd, buf, len, (off_t)off);
        return n;
    }
    if (s->use_curl)
        return curl_pread(s->info.uri, buf, len, off);
    {
        uint64_t body_off = 0;
        int rc = http_request_range(s, off, len, &body_off);
        if (rc != SIO_OK) {
            if (s->sock >= 0) {
                close(s->sock);
                s->sock = -1;
            }
            return -1;
        }
        ssize_t n = sock_read_full(s->sock, buf, len);
        if (n < 0) {
            close(s->sock);
            s->sock = -1;
            s->sock_dirty = 0;
            return -1;
        }
        if ((size_t)n != len) {
            sio_log_warn("Range 响应体偏短：期望 %zu，收到 %zd", len, n);
            close(s->sock);
            s->sock = -1;
            s->sock_dirty = 0;
            return n;
        }
        /* 200 降级路径：响应体已读完，此时才关连接，避免残留污染下一块 */
        if (s->sock_dirty) {
            close(s->sock);
            s->sock = -1;
            s->sock_dirty = 0;
        }
        return n;
    }
}

int sio_source_sha256(sio_source *s, char hex[65])
{
    sio_sha256_ctx ctx;
    uint8_t *buf = malloc(1 << 20);
    uint64_t off = 0;
    int rc = SIO_OK;

    if (!buf)
        return SIO_ERR_IO;

    sio_sha256_init(&ctx);

    if (!s->info.is_http) {
        for (;;) {
            ssize_t n = sio_pread_full(s->fd, buf, 1 << 20, (off_t)off);
            if (n < 0) {
                rc = SIO_ERR_IO;
                break;
            }
            if (n == 0)
                break;
            sio_sha256_update(&ctx, buf, (size_t)n);
            off += (uint64_t)n;
        }
    } else {
        uint64_t total = s->info.size;
        if (total == 0) {
            sio_log_warn("HTTP 源长度未知，跳过全量摘要");
            free(buf);
            hex[0] = '\0';
            return SIO_OK;
        }
        while (off < total) {
            size_t take = (total - off) > (1u << 20) ? (1u << 20) : (size_t)(total - off);
            ssize_t n = sio_source_pread(s, buf, take, off);
            if (n <= 0) {
                rc = SIO_ERR_NET;
                break;
            }
            sio_sha256_update(&ctx, buf, (size_t)n);
            off += (uint64_t)n;
        }
    }

    if (rc == SIO_OK) {
        uint8_t d[32];
        sio_sha256_final(&ctx, d);
        sio_sha256_hex(d, hex);
    }
    free(buf);
    return rc;
}
