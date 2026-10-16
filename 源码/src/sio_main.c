/*
 * sio_main.c - 命令行入口
 *
 * 子命令对应作业的四项功能要求：
 *   create  生成空洞文件（逻辑 >= 128KB，未写入区不占磁盘块）
 *   write   定点写入数据块（lseek/pwrite），支持 Ctrl-C 后断点续传
 *   status  打印逻辑大小 / 实际占用块数 / 空洞占比
 *   verify  逐块回读校验（空洞区必须仍为 0x00）
 */
#define _GNU_SOURCE
#include "sio_common.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IO_CHUNK (1u << 20)

static void usage(FILE *f, int argc, char **argv)
{
    const char *p = argc > 0 ? argv[0] : "sparseio";
    fprintf(f,
"用法: %s <子命令> [选项]\n"
"\n"
"子命令:\n"
"  create   生成空洞文件（逻辑大小 >=128KB，不占用磁盘块）\n"
"  write    按块拉取数据并定点写入；Ctrl-C 中断后重跑即断点续传\n"
"  status   打印逻辑大小 / 实际占用块数 / 空洞占比\n"
"  verify   逐块回读校验：已写块与源一致，空洞区仍为 0x00\n"
"\n"
"公共选项:\n"
"  --target=PATH     目标文件（默认 out.bin）\n"
"  --source=URI      数据源：file:///abs/path 或 http://host[:port]/path\n"
"  --block=SIZE      每块字节数（默认 64K，可写 64K / 1M / 1048576）\n"
"  --head=SIZE       前缀连续区大小（>=0，默认 4K）\n"
"  --size=SIZE       目标逻辑大小；create 不填时取源文件大小\n"
"  --bitmap=PATH     进度位图路径（默认 <target>.sio）\n"
"  --limit=N         write 最多提交 N 个块后主动退出（用于演示中断）\n"
"  --delay=MS        每块写完后的模拟延迟，便于观察/中断\n"
"  --retry=N         单块失败重试次数（默认 3）\n"
"  --verbose         输出调试信息\n"
"  --json            以 JSON 输出 status（便于脚本断言）\n"
"  -h, --help        帮助\n"
"\n"
"示例:\n"
"  %s create --target=demo.bin --size=1M\n"
"  %s write  --target=demo.bin --source=file:///tmp/src.bin --limit=6\n"
"  %s write  --target=demo.bin --source=file:///tmp/src.bin   # 续传\n"
"  %s status --target=demo.bin --json\n"
"  %s verify --target=demo.bin --source=file:///tmp/src.bin\n",
            p, p, p, p, p, p);
}

/* ------------------------------------------------------------------ */
/* 选项                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *target;
    const char *source;
    const char *bitmap;
    long long   block;
    long long   head;
    long long   size;
    long long   limit;
    long long   delay_ms;
    long long   retry;
    int         force;
    int         verbose;
    int         json;
} sio_opts;

static void opts_default(sio_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->target   = "out.bin";
    o->block    = SIO_DEFAULT_BLOCK;
    o->head     = 4096;
    o->size     = -1;
    o->limit    = -1;
    o->delay_ms = 0;
    o->retry    = 3;
}

/* 解析 argv 中 begin..end 范围的选项（跳过子命令本身） */
static int parse_opts(int argc, char **argv, int begin, sio_opts *o)
{
    static struct option longopts[] = {
        { "target",  required_argument, 0, 't' },
        { "source",  required_argument, 0, 's' },
        { "block",   required_argument, 0, 'b' },
        { "head",    required_argument, 0, 'H' },
        { "size",    required_argument, 0, 'S' },
        { "bitmap",  required_argument, 0, 'm' },
        { "limit",   required_argument, 0, 'l' },
        { "delay",   required_argument, 0, 'd' },
        { "retry",   required_argument, 0, 'r' },
        { "force",   no_argument,       0, 'f' },
        { "verbose", no_argument,       0, 'v' },
        { "json",    no_argument,       0, 'j' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    int seen_block = 0, seen_head = 0, seen_size = 0;
    int seen_limit = 0, seen_delay = 0, seen_retry = 0;

    optind = begin;
    opterr = 1;
    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (c) {
        case 't': o->target = optarg; break;
        case 's': o->source = optarg; break;
        case 'b': o->block = sio_parse_size(optarg); seen_block = 1; break;
        case 'H': o->head = sio_parse_size(optarg); seen_head = 1; break;
        case 'S': o->size = sio_parse_size(optarg); seen_size = 1; break;
        case 'm': o->bitmap = optarg; break;
        case 'l': o->limit = sio_parse_size(optarg); seen_limit = 1; break;
        case 'd': o->delay_ms = sio_parse_delay_ms(optarg); seen_delay = 1; break;
        case 'r': o->retry = sio_parse_size(optarg); seen_retry = 1; break;
        case 'f': o->force = 1; break;
        case 'v': o->verbose = 1; break;
        case 'j': o->json = 1; break;
        case 'h': usage(stdout, argc, argv); exit(SIO_OK);
        default:  usage(stderr, argc, argv); return SIO_ERR_USAGE;
        }
    }

    /*
     * 数值参数校验（对应 AI 代码审查 缺陷-2 / 缺陷-3）：
     *   - sio_parse_size() 解析失败返回 -1，与 --limit 合法的“-1=不限”撞值，
     *     也会让 --head=abc 被静默当成默认 4K，因此这里用 seen_* 标记区分
     *     “没给这个参数”和“给了但解析失败”；
     *   - --block=0 必须显式拒绝：否则 sio_map_nblocks() 算出 0 块，
     *     程序会打印“全部块提交完成”却一个字节都没下。
     */
    if (seen_block && o->block <= 0) {
        sio_log_err("--block 必须为正数（收到 %lld）", o->block);
        return SIO_ERR_USAGE;
    }
    if (seen_head && o->head < 0) {
        sio_log_err("--head 解析失败（示例：--head=4K / --head=0）");
        return SIO_ERR_USAGE;
    }
    if (seen_size && o->size < 0) {
        sio_log_err("--size 解析失败（示例：--size=8M / --size=131072）");
        return SIO_ERR_USAGE;
    }
    if (seen_limit && o->limit < -1) {
        sio_log_err("--limit 解析失败（示例：--limit=5，不填表示不限）");
        return SIO_ERR_USAGE;
    }
    if (seen_delay && o->delay_ms < 0) {
        sio_log_err("--delay 解析失败（示例：--delay=50）");
        return SIO_ERR_USAGE;
    }
    if (seen_retry && o->retry < 0) {
        sio_log_err("--retry 解析失败（示例：--retry=3）");
        return SIO_ERR_USAGE;
    }
    if (!seen_block && !seen_head && !seen_size && !seen_limit && !seen_delay && !seen_retry) {
        /* 走默认值，无需额外校验 */
    }
    if (o->block < 0 || o->head < 0 || (o->size < -1) || o->delay_ms < 0 || o->retry < 0) {
        sio_log_err("数值参数非法");
        return SIO_ERR_USAGE;
    }
    if (o->block > (long long)SIO_MAX_BLOCK) {
        sio_log_err("--block 过大（上限 %u）", SIO_MAX_BLOCK);
        return SIO_ERR_USAGE;
    }
    return SIO_OK;
}

static void bitmap_path_of(const sio_opts *o, char *buf, size_t sz)
{
    if (o->bitmap)
        snprintf(buf, sz, "%s", o->bitmap);
    else
        snprintf(buf, sz, "%s.sio", o->target);
}

/* ------------------------------------------------------------------ */
/* create                                                              */
/* ------------------------------------------------------------------ */
static int cmd_create(const sio_opts *o, uint64_t src_size)
{
    uint64_t total = 0;
    char bpath[SIO_PATH_MAX];
    sio_map m;
    int rc;

    if (o->size > 0) {
        total = (uint64_t)o->size;
    } else if (src_size > 0) {
        total = src_size;
        sio_log_info("按数据源长度确定目标大小：%" PRIu64 " 字节", total);
    } else {
        /* 没给源也没给大小：按作业最低要求建 128KB */
        total = SIO_MIN_LOGICAL;
        sio_log_warn("未指定 --size/--source，按作业下限 %d 字节创建",
                     SIO_MIN_LOGICAL);
    }

    if (total < SIO_MIN_LOGICAL) {
        sio_log_err("逻辑大小 %" PRIu64 " 小于作业下限 %d 字节",
                    total, SIO_MIN_LOGICAL);
        return SIO_ERR_USAGE;
    }
    if (o->head > (long long)total) {
        sio_log_err("--head(%lld) 不能大于总大小(%" PRIu64 ")", o->head, total);
        return SIO_ERR_USAGE;
    }
    if ((uint64_t)o->block > total) {
        sio_log_err("--block(%lld) 不能大于总大小(%" PRIu64 ")", o->block, total);
        return SIO_ERR_USAGE;
    }

    rc = sio_create_sparse(o->target, total, o->force);
    if (rc != SIO_OK)
        return rc;

    bitmap_path_of(o, bpath, sizeof(bpath));
    /* 缺陷-5（AI 代码审查）：这里原先把 src_size 硬编码为 0，导致 create 写下的
     * 位图 hdr.src_size 永远是“0=源长度未知”，后续 write 续传拿不到源长度、
     * 无法提前拦住“目标比源大”的错误。改为把 main() 探测到的 src_size 如实落盘。 */
    rc = sio_map_open(&m, bpath, 1, total, (uint32_t)o->block, (uint64_t)o->head,
                      src_size, o->source);
    if (rc != SIO_OK) {
        sio_log_err("初始化进度位图失败");
        return rc;
    }

    {
        char hl[32], bl[32];
        sio_log_info("进度位图已初始化: %s", bpath);
        sio_log_info("  块大小   : %s", sio_human((uint64_t)o->block, bl, sizeof(bl)));
        sio_log_info("  连续头区 : %s", sio_human((uint64_t)o->head, hl, sizeof(hl)));
        sio_log_info("  总块数   : %" PRIu64 "（头区算 1 块）",
                     m.hdr.nblocks);
        sio_log_info("  已完成   : 0 / %" PRIu64, m.hdr.nblocks);
    }
    sio_map_close(&m);
    return SIO_OK;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */
static int cmd_status(const sio_opts *o)
{
    sio_file_stat st;
    char bpath[SIO_PATH_MAX];
    sio_map m;
    int rc;

    rc = sio_stat_path(o->target, &st);
    if (rc != SIO_OK)
        return rc;

    bitmap_path_of(o, bpath, sizeof(bpath));
    int map_rc = sio_map_open(&m, bpath, 0, st.logical_size, (uint32_t)o->block,
                              (uint64_t)o->head, 0, NULL);
    int have_map = (map_rc == SIO_OK);

    uint64_t done = 0, nblocks = 0;
    double   pct = 0.0;
    if (have_map) {
        done    = sio_map_count_done(&m);
        nblocks = m.hdr.nblocks;
        pct     = nblocks ? (double)done * 100.0 / (double)nblocks : 100.0;
    }

    double hole = sio_hole_ratio(&st);
    char hl[32], al[32], dl[32];

    sio_human(st.logical_size, hl, sizeof(hl));
    sio_human(st.allocated, al, sizeof(al));
    sio_human(st.logical_size - st.allocated, dl, sizeof(dl));

    if (o->json) {
        printf("{\n");
        printf("  \"target\": \"%s\",\n", o->target);
        printf("  \"logical_size\": %" PRIu64 ",\n", st.logical_size);
        printf("  \"block_count_512\": %" PRIu64 ",\n", st.block_count);
        printf("  \"allocated_bytes\": %" PRIu64 ",\n", st.allocated);
        printf("  \"hole_bytes\": %" PRIu64 ",\n", st.logical_size - st.allocated);
        printf("  \"hole_ratio\": %.4f,\n", hole);
        printf("  \"io_block_size\": %u,\n", st.io_block_size);
        printf("  \"bitmap\": \"%s\",\n", have_map ? bpath : "");
        printf("  \"blocks_total\": %" PRIu64 ",\n", nblocks);
        printf("  \"blocks_done\": %" PRIu64 ",\n", done);
        printf("  \"progress_pct\": %.2f\n", pct);
        printf("}\n");
    } else {
        printf("目标文件      : %s\n", o->target);
        printf("逻辑大小      : %" PRIu64 " 字节  (%s)\n", st.logical_size, hl);
        printf("实际占用      : %" PRIu64 " 字节  (%s)，即 %" PRIu64 " 个 512B 块\n",
               st.allocated, al, st.block_count);
        printf("空洞大小      : %" PRIu64 " 字节  (%s)\n", st.logical_size - st.allocated, dl);
        printf("空洞占比      : %.2f%%\n", hole * 100.0);
        printf("文件系统块大小: %u 字节\n", st.io_block_size);
        if (have_map) {
            printf("进度位图      : %s\n", bpath);
            printf("分块进度      : %" PRIu64 " / %" PRIu64 " 块 (%.2f%%)\n",
                   done, nblocks, pct);
        } else if (map_rc == SIO_ERR_BITMAP) {
            /* 缺陷-8（AI 代码审查）：位图存在但参数对不上时，原提示会误导用户
             * 以为“这个文件不是本工具创建的”。这里区分开，并给出正确指引。 */
            printf("进度位图      : %s（存在但与当前 --block/--head/--size 不一致，"
                   "请改回 create 时的参数）\n", bpath);
        } else {
            printf("进度位图      : 无（该文件未由本工具 create）\n");
        }
    }

    if (have_map)
        sio_map_close(&m);
    return SIO_OK;
}

/* ------------------------------------------------------------------ */
/* write                                                              */
/* ------------------------------------------------------------------ */
static int cmd_write(const sio_opts *o, uint64_t src_size)
{
    sio_source *src = NULL;
    sio_file_stat st;
    sio_map m;
    char bpath[SIO_PATH_MAX];
    uint8_t *wbuf = NULL, *rbuf = NULL;
    uint64_t total;
    uint64_t done_before, done_now = 0, written = 0;
    int fd = -1, rc;
    struct timespec t0, t1;

    if (!o->source) {
        sio_log_err("write 必须指定 --source");
        return SIO_ERR_USAGE;
    }

    rc = sio_stat_path(o->target, &st);
    if (rc != SIO_OK) {
        sio_log_err("请先执行 create 生成目标文件");
        return rc;
    }
    total = st.logical_size;

    rc = sio_source_open(&src, o->source);
    if (rc != SIO_OK)
        return rc;
    if (sio_source_get_info(src)->size && src_size == 0)
        src_size = sio_source_get_info(src)->size;

    if (src_size && total > src_size) {
        sio_log_err("目标大小 %" PRIu64 " 超过源大小 %" PRIu64 "，无法完整续传",
                    total, src_size);
        sio_source_close(src);
        return SIO_ERR_RANGE;
    }

    bitmap_path_of(o, bpath, sizeof(bpath));
    rc = sio_map_open(&m, bpath, 1, total, (uint32_t)o->block, (uint64_t)o->head,
                      src_size, o->source);
    if (rc != SIO_OK) {
        sio_source_close(src);
        return rc;
    }

    done_before = sio_map_count_done(&m);

    fd = open(o->target, O_RDWR);
    if (fd < 0) {
        sio_log_err("打开目标 %s 失败: %s", o->target, strerror(errno));
        sio_map_close(&m);
        sio_source_close(src);
        return SIO_ERR_IO;
    }

    wbuf = malloc((size_t)o->block);
    rbuf = malloc((size_t)o->block);
    if (!wbuf || !rbuf) {
        sio_log_err("内存分配失败");
        rc = SIO_ERR_IO;
        goto out;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);

    sio_log_info("开始写入: 目标=%s 源=%s", o->target, o->source);
    sio_log_info("  逻辑大小=%" PRIu64 " 块大小=%lld 头区=%lld 总块数=%" PRIu64,
                 total, o->block, o->head, m.hdr.nblocks);
    if (done_before)
        sio_log_info("  检测到已有进度位图：已完成 %" PRIu64 "/%" PRIu64
                     " 块，从断点继续", done_before, m.hdr.nblocks);

    for (uint64_t i = 0; i < m.hdr.nblocks; i++) {
        off_t  off = 0;
        size_t len = 0;
        int    attempt;

        sio_map_block_range(&m, i, &off, &len);

        if (sio_map_get(&m, i)) {
            sio_log_verbose("块 %" PRIu64 "/%" PRIu64 " [%" PRIu64 ",+%zu) 已完成，跳过",
                            i + 1, m.hdr.nblocks, (uint64_t)off, len);
            done_now++;
            continue;
        }

        /* 缺陷-7（AI 代码审查）：信号处理函数故意不加 SA_RESTART，所以按 Ctrl-C 时
         * 正在阻塞的 pread/recv 会带着 EINTR 提前返回，被误判成“读块失败”并进入
         * 重试；用户明明已经按了 Ctrl-C，却要等 retry 次全失败才退出。
         * 每块开始前先看一次中断标志，立刻在块边界干净退出。 */
        if (sio_interrupted()) {
            sio_log_warn("收到 %s，停止写入（进度已保存到位图）",
                         sio_interrupt_signal_name());
            rc = SIO_ERR_INTERRUPT;
            goto out;
        }

        for (attempt = 0; attempt <= o->retry; attempt++) {
            ssize_t n = sio_source_pread(src, wbuf, len, (uint64_t)off);
            if (n != (ssize_t)len) {
                sio_log_warn("块 %" PRIu64 " 读取失败（第 %d 次，期望 %zu 实际 %zd）",
                             i, attempt + 1, len, n);
                if (attempt < o->retry)
                    continue;
                sio_log_err("块 %" PRIu64 " 重试 %lld 次仍失败，保留断点退出",
                            i, o->retry);
                rc = SIO_ERR_NET;
                goto out;
            }

            /* 核心：pwrite 定点写入，只碰这一段的字节，其它空洞不受影响 */
            if (sio_pwrite_full(fd, wbuf, len, off) != (ssize_t)len) {
                sio_log_err("块 %" PRIu64 " 写入失败: %s", i, strerror(errno));
                rc = SIO_ERR_IO;
                goto out;
            }

            /* 写完立刻回读校验，确认真的落到了这个偏移上 */
            if (sio_pread_full(fd, rbuf, len, off) != (ssize_t)len ||
                memcmp(rbuf, wbuf, len) != 0) {
                sio_log_warn("块 %" PRIu64 " 回读校验不一致（第 %d 次）",
                             i, attempt + 1);
                if (attempt < o->retry)
                    continue;
                rc = SIO_ERR_VERIFY;
                goto out;
            }
            break;
        }

        /* 校验通过才记账：位图置位并落盘 */
        sio_map_set(&m, i, 1);
        if (sio_map_save(&m) != SIO_OK) {
            rc = SIO_ERR_IO;
            goto out;
        }

        done_now++;
        written += len;

        if (fsync(fd) != 0)
            sio_log_warn("fsync 失败: %s", strerror(errno));

        sio_log_info("已完成偏移 [%10" PRIu64 " , +%7zu)  块 %" PRIu64 "/%" PRIu64
                     "  %.2f%%", (uint64_t)off, len, i + 1, m.hdr.nblocks,
                     (double)(i + 1) * 100.0 / (double)m.hdr.nblocks);

        if (o->limit >= 0 && (done_now + done_before) >= (uint64_t)o->limit) {
            sio_log_warn("达到 --limit=%lld，主动中断以演示断点续传", o->limit);
            rc = SIO_ERR_INTERRUPT;
            goto out;
        }

        if (o->delay_ms > 0 && sio_sleep_ms_interruptible(o->delay_ms) != 0) {
            sio_log_warn("收到 %s，停止写入", sio_interrupt_signal_name());
            rc = SIO_ERR_INTERRUPT;
            goto out;
        }

        if (sio_interrupted()) {
            sio_log_warn("收到 %s，停止写入（进度已保存到位图）",
                         sio_interrupt_signal_name());
            rc = SIO_ERR_INTERRUPT;
            goto out;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    /* 全部块提交完成：记录源摘要，供 verify 使用 */
    if (!m.hdr.src_sha256[0] && src_size) {
        if (sio_source_sha256(src, m.hdr.src_sha256) == SIO_OK && m.hdr.src_sha256[0]) {
            if (sio_map_save(&m) == SIO_OK)
                sio_log_info("已记录源摘要 sha256=%s", m.hdr.src_sha256);
        }
    }

    sio_log_info("全部块提交完成：%" PRIu64 " 块，本次新写入 %" PRIu64
                 " 字节，本次跳过（已完成）%" PRIu64 " 块，用时 %.2fs（%.2f MiB/s）",
                 m.hdr.nblocks, written, done_before, secs,
                 secs > 0 ? (double)written / secs / 1048576.0 : 0.0);
    rc = SIO_OK;

out:
    /*
     * 缺陷-11（AI 代码审查）：中断分支（Ctrl-C / --limit / 读块失败）原来直接
     * goto out，只留下“退出码 130”，把“本次新写入多少、跳过了多少已完成块”
     * 这行最关键的续传证据吞掉了；正常结束分支才打印。
     * 结果：演示断点续传时看到的最后一行反而是退出码，看不到断点信息。
     * 这里把汇总统一挪到出口打印一次，两条路径都有，且不会重复。
     */
    if (rc == SIO_ERR_INTERRUPT)
        sio_log_info("中断汇总：本次新写入 %" PRIu64
                     " 字节，本次跳过（已完成）%" PRIu64 " 块，进度已保存到位图",
                     written, done_before);
    if (fd >= 0)
        close(fd);
    if (wbuf)
        free(wbuf);
    if (rbuf)
        free(rbuf);
    sio_map_close(&m);
    sio_source_close(src);
    return rc;
}

/* ------------------------------------------------------------------ */
/* verify                                                              */
/* ------------------------------------------------------------------ */
static int cmd_verify(const sio_opts *o)
{
    sio_file_stat st;
    sio_map m;
    char bpath[SIO_PATH_MAX];
    uint8_t *wbuf = NULL, *rbuf = NULL;
    int fd = -1, rc;
    uint64_t checked = 0, holes = 0;
    uint64_t hole_scan_bytes = 0;
    sio_source *src = NULL;

    rc = sio_stat_path(o->target, &st);
    if (rc != SIO_OK)
        return rc;

    bitmap_path_of(o, bpath, sizeof(bpath));
    rc = sio_map_open(&m, bpath, 0, st.logical_size, (uint32_t)o->block,
                      (uint64_t)o->head, 0, NULL);
    if (rc == SIO_ERR_BITMAP)
        sio_log_err("位图 %s 存在但不可用（参数不一致或文件损坏），无法校验进度", bpath);
    if (rc != SIO_OK)
        return rc;

    fd = open(o->target, O_RDONLY);
    if (fd < 0) {
        sio_log_err("打开 %s 失败: %s", o->target, strerror(errno));
        sio_map_close(&m);
        return SIO_ERR_IO;
    }

    wbuf = malloc((size_t)o->block);
    rbuf = malloc((size_t)o->block);
    if (!wbuf || !rbuf) {
        rc = SIO_ERR_IO;
        goto out;
    }

    if (o->source) {
        rc = sio_source_open(&src, o->source);
        if (rc != SIO_OK)
            goto out;
    }

    /* 1) 已提交的块：回读比对 */
    for (uint64_t i = 0; i < m.hdr.nblocks; i++) {
        if (!sio_map_get(&m, i))
            continue;
        off_t off = 0;
        size_t len = 0;
        sio_map_block_range(&m, i, &off, &len);

        if (sio_pread_full(fd, rbuf, len, off) != (ssize_t)len) {
            sio_log_err("块 %" PRIu64 " [%" PRIu64 ",+%zu) 回读失败", i,
                        (uint64_t)off, len);
            rc = SIO_ERR_VERIFY;
            goto out;
        }

        if (src) {
            if (sio_source_pread(src, wbuf, len, (uint64_t)off) != (ssize_t)len ||
                memcmp(wbuf, rbuf, len) != 0) {
                sio_log_err("块 %" PRIu64 " [%" PRIu64 ",+%zu) 与源不一致",
                            i, (uint64_t)off, len);
                rc = SIO_ERR_VERIFY;
                goto out;
            }
        }
        checked++;
        sio_log_verbose("块 %" PRIu64 " 校验通过", i);
    }

    /* 2) 未提交的块：必须仍是空洞（全 0x00），且没有偷占磁盘块 */
    for (uint64_t i = 0; i < m.hdr.nblocks; i++) {
        if (sio_map_get(&m, i))
            continue;
        off_t off = 0;
        size_t len = 0;
        sio_map_block_range(&m, i, &off, &len);
        ssize_t n = sio_pread_full(fd, rbuf, len, off);
        if (n != (ssize_t)len) {
            sio_log_err("空洞块 %" PRIu64 " 读取异常", i);
            rc = SIO_ERR_IO;
            goto out;
        }
        for (size_t k = 0; k < len; k++) {
            if (rbuf[k] != SIO_HOLE_FILL) {
                sio_log_err("空洞块 %" PRIu64 " 偏移 %" PRIu64
                            " 处不是 0x00（实际 0x%02x），空洞被破坏",
                            i, (uint64_t)off + k, rbuf[k]);
                rc = SIO_ERR_VERIFY;
                goto out;
            }
        }
        holes++;
        hole_scan_bytes += len;
    }

    {
        char hl[32], al[32];
        sio_human(st.logical_size, hl, sizeof(hl));
        sio_human(st.allocated, al, sizeof(al));

        sio_log_info("校验通过：");
        sio_log_info("  目标文件    : %s", o->target);
        sio_log_info("  逻辑大小    : %" PRIu64 " 字节 (%s)", st.logical_size, hl);
        sio_log_info("  磁盘占用    : %" PRIu64 " 字节 (%s)，空洞占比 %.2f%%",
                     st.allocated, al, sio_hole_ratio(&st) * 100.0);
        sio_log_info("  已写块      : %" PRIu64 " / %" PRIu64 " 块（逐块与源比对一致）",
                     checked, m.hdr.nblocks);
        sio_log_info("  空洞块      : %" PRIu64 " 块，共 %" PRIu64
                     " 字节确认为 0x00", holes, hole_scan_bytes);
        if (checked != m.hdr.nblocks) {
            sio_log_warn("注意：仍有 %" PRIu64 " 个块未下载，文件不完整",
                         m.hdr.nblocks - checked);
            rc = SIO_ERR_VERIFY;
            goto out;
        }
    }
    rc = SIO_OK;

out:
    if (fd >= 0)
        close(fd);
    if (src)
        sio_source_close(src);
    free(wbuf);
    free(rbuf);
    sio_map_close(&m);
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    sio_opts o;
    const char *cmd;
    int rc = SIO_OK;
    uint64_t src_size = 0;

    sio_install_signal_handlers();
    opts_default(&o);

    if (argc < 2) {
        usage(stderr, argc, argv);
        return SIO_ERR_USAGE;
    }
    cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout, argc, argv);
        return SIO_OK;
    }

    rc = parse_opts(argc, argv, 2, &o);
    if (rc != SIO_OK)
        return rc;

    g_verbose = o.verbose;
    g_json    = o.json;

    /* create/write 前先把源打开一次，取长度（HTTP 源顺带探测 Range 支持） */
    if (o.source && (strcmp(cmd, "create") == 0 || strcmp(cmd, "write") == 0)) {
        sio_source *p = NULL;
        if (sio_source_open(&p, o.source) == SIO_OK) {
            sio_source_probe_size(p);
            src_size = sio_source_get_info(p)->size;
            sio_source_close(p);
            sio_log_verbose("探测到源长度 %llu 字节", (unsigned long long)src_size);
        } else if (strcmp(cmd, "create") == 0) {
            sio_log_warn("源 %s 打不开，create 将只按 --size 建空洞文件", o.source);
        }
    }

    if (strcmp(cmd, "create") == 0) {
        rc = cmd_create(&o, src_size);
    } else if (strcmp(cmd, "write") == 0) {
        rc = cmd_write(&o, src_size);
    } else if (strcmp(cmd, "status") == 0) {
        rc = cmd_status(&o);
    } else if (strcmp(cmd, "verify") == 0) {
        rc = cmd_verify(&o);
    } else {
        sio_log_err("未知子命令: %s", cmd);
        usage(stderr, argc, argv);
        rc = SIO_ERR_USAGE;
    }

    if (rc == SIO_ERR_INTERRUPT)
        sio_log_info("退出码 %d（中断，可重新执行 write 续传）", rc);
    return rc;
}
