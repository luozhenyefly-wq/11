/*
 * sio_bitmap.c - 进度位图 sidecar 文件
 *
 * 断点续传的核心：每写完并校验通过一个块，就把对应 bit 置 1 并落盘。
 * 进程被 Ctrl-C / kill 后重新启动，只要读到这个文件就知道哪些块已经完成，
 * 只补写剩下的块，而不是从头再来。
 *
 * 文件布局：
 *   [ sio_map_hdr ][ ceil(nblocks/8) 字节位图 ]
 */
#define _GNU_SOURCE
#include "sio_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t sio_map_nblocks(uint64_t total_size, uint32_t block_size, uint64_t head_size)
{
    uint64_t body;

    if (total_size == 0)
        return 0;
    if (block_size == 0)
        return 0;

    /*
     * 块数 = 头区块(若 head_size>0) + 数据块数。
     *
     * 缺陷-10（AI 代码审查，本次实测复现的最严重缺陷）：
     * 原实现直接 return ceil((total-head)/block_size)，把头部当成了“第 0 块”，
     * 于是数据块数量比实际需要少 1。当 head_size 不是 block_size 的整数倍时，
     * sio_map_block_range() 的 end 是按“数据块序号 * block_size”递推的，
     * 末尾那一段不足一块的数据就再也落不到任何块上 —— 表现为：
     * 程序报告“全部块提交完成”、退出码 0，但文件尾部若干字节永远是空洞。
     * 实测：total=143417, head=100, block=64K 时只写了 131172 字节，
     * 尾部 12245 字节丢失（T5.3 用例）。
     * 修法：数据块按 (total - head + block_size - 1) / block_size 计算，
     * 最后一块允许不满；sio_map_block_range() 已把 end 收在 total_size 内，
     * 因此最后一块会自然变成“尾块”。
     */
    if (head_size > total_size)
        head_size = total_size;
    body = total_size - head_size;

    return (head_size > 0 ? 1 : 0) + (body + block_size - 1) / block_size;
}

void sio_map_block_range(const sio_map *m, uint64_t i, off_t *off, size_t *len)
{
    uint64_t head = m->hdr.head_size;
    uint64_t nb   = m->hdr.nblocks;

    if (i >= nb) {
        *off = 0;
        *len = 0;
        return;
    }

    if (i == 0 && head > 0) {
        *off = 0;
        *len = (size_t)head;
        return;
    }

    {
        uint64_t start = head + (i - (head > 0 ? 1 : 0)) * (uint64_t)m->hdr.block_size;
        uint64_t end   = start + m->hdr.block_size;
        if (end > m->hdr.total_size)
            end = m->hdr.total_size;
        *off = (off_t)start;
        *len = (size_t)(end - start);
    }
}

static size_t map_bytes(const sio_map *m)
{
    return (size_t)((m->hdr.nblocks + 7) / 8);
}

int sio_map_open(sio_map *m, const char *path, int create,
                 uint64_t total_size, uint32_t block_size, uint64_t head_size,
                 uint64_t src_size, const char *src_uri)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        sio_map_hdr h;
        ssize_t n = pread(fd, &h, sizeof(h), 0);

        if (n != (ssize_t)sizeof(h)) {
            sio_log_err("位图 %s 头部不完整（读到 %zd 字节）", path, n);
            close(fd);
            return SIO_ERR_BITMAP;
        }
        if (memcmp(h.magic, SIO_MAP_MAGIC, SIO_MAP_MAGIC_LEN) != 0) {
            sio_log_err("位图 %s 魔数非法，不是本工具的进度文件", path);
            close(fd);
            return SIO_ERR_BITMAP;
        }
        if (h.version != 1) {
            sio_log_err("位图版本 %u 不支持", h.version);
            close(fd);
            return SIO_ERR_BITMAP;
        }
        if (h.total_size != total_size || h.block_size != block_size ||
            h.head_size != head_size) {
            sio_log_err("位图与当前参数不一致：位图(total=%llu block=%u head=%llu) "
                        "命令(total=%llu block=%u head=%llu)",
                        (unsigned long long)h.total_size, h.block_size,
                        (unsigned long long)h.head_size,
                        (unsigned long long)total_size, block_size,
                        (unsigned long long)head_size);
            sio_log_err("请删除 %s 后重新 create，或改回原参数", path);
            close(fd);
            return SIO_ERR_BITMAP;
        }

        m->hdr = h;
        m->bits = calloc(map_bytes(m) ? map_bytes(m) : 1, 1);
        if (!m->bits) {
            close(fd);
            return SIO_ERR_IO;
        }
        size_t want = map_bytes(m);
        n = pread(fd, m->bits, want, (off_t)sizeof(sio_map_hdr));
        if (n < 0 || (size_t)n != want) {
            sio_log_err("位图 %s 位数据不完整（期望 %zu 字节，读到 %zd）", path, want, n);
            free(m->bits);
            m->bits = NULL;
            close(fd);
            return SIO_ERR_BITMAP;
        }
        close(fd);
        sio_log_verbose("位图已加载：%s，共 %llu 块，已完成 %llu 块",
                        path, (unsigned long long)m->hdr.nblocks,
                        (unsigned long long)sio_map_count_done(m));
        return SIO_OK;
    }

    if (!create) {
        sio_log_err("位图 %s 不存在（需要先执行 create）", path);
        return SIO_ERR_BITMAP;
    }

    /* 新建 */
    memcpy(m->hdr.magic, SIO_MAP_MAGIC, SIO_MAP_MAGIC_LEN);
    m->hdr.version    = 1;
    m->hdr.block_size = block_size;
    m->hdr.total_size = total_size;
    m->hdr.head_size  = head_size;
    m->hdr.src_size   = src_size;
    snprintf(m->hdr.src_uri, sizeof(m->hdr.src_uri), "%s", src_uri ? src_uri : "");
    m->hdr.src_sha256[0] = '\0';
    m->hdr.nblocks    = sio_map_nblocks(total_size, block_size, head_size);

    m->bits = calloc(map_bytes(m) ? map_bytes(m) : 1, 1);
    if (!m->bits)
        return SIO_ERR_IO;

    m->dirty = 1;
    sio_log_verbose("位图新建：%s，共 %llu 块", path,
                    (unsigned long long)m->hdr.nblocks);
    return sio_map_save(m);
}

int sio_map_save(sio_map *m)
{
    char tmp[SIO_PATH_MAX + 16];
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.tmp", m->path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        sio_log_err("无法写入位图 %s: %s", tmp, strerror(errno));
        return SIO_ERR_IO;
    }

    size_t hdr_sz = sizeof(sio_map_hdr);
    size_t bit_sz = map_bytes(m);

    if (sio_pwrite_full(fd, &m->hdr, hdr_sz, 0) != (ssize_t)hdr_sz ||
        sio_pwrite_full(fd, m->bits, bit_sz, (off_t)hdr_sz) != (ssize_t)bit_sz) {
        sio_log_err("写位图失败: %s", strerror(errno));
        close(fd);
        unlink(tmp);
        return SIO_ERR_IO;
    }

    /* 数据落盘后再 rename，保证位图要么是旧的完整版，要么是新的完整版 */
    if (fsync(fd) != 0)
        sio_log_warn("位图 fsync 失败: %s", strerror(errno));
    close(fd);

    if (rename(tmp, m->path) != 0) {
        sio_log_err("位图 rename 失败: %s", strerror(errno));
        unlink(tmp);
        return SIO_ERR_IO;
    }

    /* 缺陷-9（AI 代码审查）：rename() 只保证“原子替换”，不保证“已经持久化”。
     * 位图所在目录的 dentry 若未落盘，掉电后可能回退到旧名字，与前面
     * “位图要么是旧的完整版，要么是新的完整版”的承诺不符。补一次目录 fsync。
     * 失败只告警：文件系统不支持目录 fsync 时不应让续传整体失败。 */
    {
        char dirbuf[SIO_PATH_MAX];
        snprintf(dirbuf, sizeof(dirbuf), "%s", m->path);
        char *slash = strrchr(dirbuf, '/');
        if (slash) {
            *slash = '\0';
            int dfd = open(dirbuf[0] ? dirbuf : "/", O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                if (fsync(dfd) != 0)
                    sio_log_warn("位图目录 fsync 失败（不影响续传）: %s",
                                 strerror(errno));
                close(dfd);
            }
        }
    }

    m->dirty = 0;
    return SIO_OK;
}

int sio_map_get(const sio_map *m, uint64_t i)
{
    if (i >= m->hdr.nblocks)
        return 0;
    return (m->bits[i / 8] >> (i % 8)) & 1;
}

void sio_map_set(sio_map *m, uint64_t i, int v)
{
    if (i >= m->hdr.nblocks)
        return;
    uint8_t mask = (uint8_t)(1u << (i % 8));
    if (v)
        m->bits[i / 8] |= mask;
    else
        m->bits[i / 8] &= (uint8_t)~mask;
    m->dirty = 1;
}

uint64_t sio_map_count_done(const sio_map *m)
{
    uint64_t c = 0;
    for (uint64_t i = 0; i < m->hdr.nblocks; i++)
        if (sio_map_get(m, i))
            c++;
    return c;
}

void sio_map_close(sio_map *m)
{
    if (!m)
        return;
    free(m->bits);
    m->bits = NULL;
}
