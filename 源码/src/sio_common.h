/*
 * sio_common.h - 空洞文件「断点续传」下载器 公共定义
 *
 * 编程作业（AI 协作版）- 基于文件 IO 的空洞文件「断点续传」下载器
 * 平台：Ubuntu 22.04.5 LTS (x86_64)
 *
 * 本文件只放类型 / 常量 / 公共函数声明，不放实现。
 */
#ifndef SIO_COMMON_H
#define SIO_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* 程序信息                                                            */
/* ------------------------------------------------------------------ */
#define SIO_NAME    "sparseio"
#define SIO_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* 默认参数                                                            */
/* ------------------------------------------------------------------ */
#define SIO_DEFAULT_BLOCK   (64 * 1024)   /* 单个写块大小 64 KiB        */
#define SIO_MIN_LOGICAL     (128 * 1024)  /* 作业要求：逻辑大小 >=128KB */
#define SIO_MAX_BLOCK       (64u * 1024 * 1024)

/* 作业要求「不破坏其余空洞」：空洞区在被真正写入前必须保持零字节 */
#define SIO_HOLE_FILL       0x00

#define SIO_URL_MAX         2048
#define SIO_PATH_MAX        4096

/* ------------------------------------------------------------------ */
/* 错误码（mockable，方便脚本断言）                                     */
/* ------------------------------------------------------------------ */
enum {
    SIO_OK              = 0,
    SIO_ERR_USAGE       = 2,
    SIO_ERR_IO          = 3,
    SIO_ERR_RANGE       = 4,
    SIO_ERR_BITMAP      = 5,
    SIO_ERR_NET         = 6,
    SIO_ERR_VERIFY      = 7,
    SIO_ERR_INTERRUPT   = 130      /* Ctrl-C */
};

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */
extern int g_verbose;
extern int g_json;

void sio_log_info(const char *fmt, ...);
void sio_log_warn(const char *fmt, ...);
void sio_log_err(const char *fmt, ...);
void sio_log_verbose(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* 通用小工具                                                          */
/* ------------------------------------------------------------------ */
/* 人类可读字节数，写入 buf（至少 32 字节），返回 buf */
const char *sio_human(uint64_t bytes, char *buf, size_t bufsz);

/* 解析 "64K" / "4M" / "1048576" 形式的大小，失败返回 -1 */
long long sio_parse_size(const char *s);

/* 解析 "100.5" / "100" 形式毫秒延迟，失败返回 -1 */
long long sio_parse_delay_ms(const char *s);

/* Ctrl-C 处理：安装 SIGINT/SIGTERM 置位，返回是否已请求中断 */
void        sio_install_signal_handlers(void);
int         sio_interrupted(void);
const char *sio_interrupt_signal_name(void);

/* 忙等 delay_ms 毫秒，期间可被中断打断，返回 0 正常 / -1 被中断 */
int sio_sleep_ms_interruptible(long long delay_ms);

/* 把 [begin,end) 全部写/读满，返回实际字节数（<0 出错） */
ssize_t sio_pwrite_full(int fd, const void *buf, size_t len, off_t off);
ssize_t sio_pread_full(int fd, void *buf, size_t len, off_t off);

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sio_sha256_ctx;

void sio_sha256_init(sio_sha256_ctx *ctx);
void sio_sha256_update(sio_sha256_ctx *ctx, const void *data, size_t len);
void sio_sha256_final(sio_sha256_ctx *ctx, uint8_t out[32]);

/* 32 字节摘要 -> 64 字符小写 hex（out 至少 65 字节） */
void sio_sha256_hex(const uint8_t digest[32], char out[65]);

/* ------------------------------------------------------------------ */
/* 进度位图（sidecar 文件，断点续传的“进度存档”）                       */
/* ------------------------------------------------------------------ */
#define SIO_MAP_MAGIC   "SIOBITMAP\x01"
#define SIO_MAP_MAGIC_LEN 10

typedef struct {
    char     magic[SIO_MAP_MAGIC_LEN];
    uint32_t version;        /* 1 */
    uint32_t block_size;     /* 每块字节数 */
    uint64_t nblocks;        /* ceil(total_size / block_size) */
    uint64_t total_size;     /* 目标文件逻辑大小 */
    uint64_t head_size;      /* 前 head_size 字节为连续区（非空洞） */
    uint64_t src_size;       /* 数据源可信长度（未知填 0） */
    char     src_uri[SIO_URL_MAX];
    char     src_sha256[65];
} sio_map_hdr;

typedef struct {
    sio_map_hdr hdr;
    uint8_t    *bits;        /* nblocks 个 bit，1 = 该块已写完并通过校验 */
    char        path[SIO_PATH_MAX];
    int         dirty;
} sio_map;

/* 根据总大小/块大小/头部大小计算块数 */
uint64_t sio_map_nblocks(uint64_t total_size, uint32_t block_size, uint64_t head_size);

/* 计算第 i 块的偏移与长度（最后一块可能不满） */
void sio_map_block_range(const sio_map *m, uint64_t i, off_t *off, size_t *len);

int  sio_map_open(sio_map *m, const char *path, int create,
                  uint64_t total_size, uint32_t block_size, uint64_t head_size,
                  uint64_t src_size, const char *src_uri);
int  sio_map_save(sio_map *m);
int  sio_map_get(const sio_map *m, uint64_t i);
void sio_map_set(sio_map *m, uint64_t i, int v);
uint64_t sio_map_count_done(const sio_map *m);
void sio_map_close(sio_map *m);

/* ------------------------------------------------------------------ */
/* 数据源（file:// 本地文件 或 http(s):// 远端）                        */
/* ------------------------------------------------------------------ */
typedef struct sio_source sio_source;

typedef struct {
    int      is_http;      /* 1 = 支持 Range 的 HTTP(S) 源 */
    uint64_t size;         /* 源长度；未知填 0 */
    char     uri[SIO_URL_MAX];
    char     scheme[16];
} sio_source_info;

/* 打开数据源；url 形如 file:///path 或 http://host:port/path */
int  sio_source_open(sio_source **out, const char *url);
void sio_source_close(sio_source *s);

/* 从 off 读取 len 字节；返回实际读取字节数，<0 表示错误 */
ssize_t sio_source_pread(sio_source *s, void *buf, size_t len, uint64_t off);

/* HTTP 源：探测总长度并确认是否支持 Range；本地源直接返回 */
int sio_source_probe_size(sio_source *s);

const sio_source_info *sio_source_get_info(const sio_source *s);

/* 读取整个源并计算 SHA-256（源可能很大，流式处理） */
int sio_source_sha256(sio_source *s, char hex[65]);

/* ------------------------------------------------------------------ */
/* 稀疏文件工具                                                        */
/* ------------------------------------------------------------------ */
/* 目标文件实际统计 */
typedef struct {
    uint64_t logical_size;   /* st_size：逻辑大小 */
    uint64_t allocated;      /* st_blocks * 512：实际占磁盘字节数 */
    uint64_t block_count;    /* st_blocks：512B 块数 */
    uint32_t io_block_size;  /* st_blksize */
} sio_file_stat;

int sio_stat_path(const char *path, sio_file_stat *st);

/* 空洞占比（0.0~1.0）：1 - allocated/logical */
double sio_hole_ratio(const sio_file_stat *st);

/* 稀疏文件的关键：创建时用 ftruncate 把逻辑大小撑开，
 * 此时不落任何数据块；失败则用 lseek 到末尾写 1 字节再截断。 */
int sio_create_sparse(const char *path, uint64_t size, int force);

#endif /* SIO_COMMON_H */
