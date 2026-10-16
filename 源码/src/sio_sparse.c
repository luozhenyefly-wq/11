/*
 * sio_sparse.c - 稀疏文件创建与统计
 *
 * 关键点：用 ftruncate() 把文件「逻辑大小」撑到目标大小。
 * 文件系统（ext4）只记录长度，不分配任何数据块，这些区间就是「空洞」，
 * 读出来全是 0x00 字节，但 du 统计的磁盘占用是 0。
 */
#define _GNU_SOURCE
#include "sio_common.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int sio_stat_path(const char *path, sio_file_stat *st)
{
    struct stat sb;

    if (stat(path, &sb) != 0) {
        sio_log_err("stat %s 失败: %s", path, strerror(errno));
        return SIO_ERR_IO;
    }
    if (!S_ISREG(sb.st_mode)) {
        sio_log_err("%s 不是普通文件", path);
        return SIO_ERR_IO;
    }

    st->logical_size = (uint64_t)sb.st_size;
    st->block_count  = (uint64_t)sb.st_blocks;
    st->allocated    = (uint64_t)sb.st_blocks * 512u;
    st->io_block_size = (uint32_t)sb.st_blksize;
    return SIO_OK;
}

double sio_hole_ratio(const sio_file_stat *st)
{
    if (st->logical_size == 0)
        return 0.0;
    if (st->allocated >= st->logical_size)
        return 0.0;
    return 1.0 - (double)st->allocated / (double)st->logical_size;
}

int sio_create_sparse(const char *path, uint64_t size, int force)
{
    int fd;
    int fallback = 0;

    fd = open(path, O_RDWR | O_CREAT | (force ? O_TRUNC : O_EXCL), 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            sio_log_err("目标 %s 已存在（加 --force 覆盖，或换个路径）", path);
        else
            sio_log_err("创建 %s 失败: %s", path, strerror(errno));
        return SIO_ERR_IO;
    }
    if (force && ftruncate(fd, 0) != 0) {
        sio_log_err("truncate %s 失败: %s", path, strerror(errno));
        close(fd);
        return SIO_ERR_IO;
    }

    /* 首选：ftruncate 直接扩展逻辑大小 —— 不写任何数据，天然形成空洞 */
    if (ftruncate(fd, (off_t)size) != 0) {
        sio_log_warn("ftruncate 失败(%s)，回退 lseek+write 方案", strerror(errno));

        /* 回退方案：lseek 到 size-1 写 1 个字节，再把长度截回 size。
         * 这样中间区间同样是空洞，只是多占了 1 个数据块。 */
        if (size == 0) {
            close(fd);
            return SIO_OK;
        }
        if (lseek(fd, (off_t)(size - 1), SEEK_SET) < 0) {
            sio_log_err("lseek 失败: %s", strerror(errno));
            close(fd);
            return SIO_ERR_IO;
        }
        uint8_t z = SIO_HOLE_FILL;
        if (write(fd, &z, 1) != 1) {
            sio_log_err("尾字节写入失败: %s", strerror(errno));
            close(fd);
            return SIO_ERR_IO;
        }
        if (ftruncate(fd, (off_t)size) != 0) {
            sio_log_err("回退后 truncate 失败: %s", strerror(errno));
            close(fd);
            return SIO_ERR_IO;
        }
        fallback = 1;
    }

    if (fsync(fd) != 0)
        sio_log_warn("fsync 失败: %s", strerror(errno));
    close(fd);

    {
        sio_file_stat st;
        char hl[32], al[32];
        if (sio_stat_path(path, &st) != 0)
            return SIO_ERR_IO;

        if (st.logical_size != size) {
            sio_log_err("逻辑大小校验失败：期望 %llu，实际 %llu",
                        (unsigned long long)size, (unsigned long long)st.logical_size);
            return SIO_ERR_IO;
        }
        if (fallback)
            sio_log_warn("使用 lseek+write 回退路径创建（磁盘占用会多 1 个数据块）");

        sio_human(st.logical_size, hl, sizeof(hl));
        sio_human(st.allocated, al, sizeof(al));

        sio_log_info("稀疏文件已创建: %s", path);
        sio_log_info("  逻辑大小 : %s (%llu 字节)",
                     hl, (unsigned long long)st.logical_size);
        sio_log_info("  实际占用 : %s (%llu 字节, %llu 个 512B 块)",
                     al, (unsigned long long)st.allocated,
                     (unsigned long long)st.block_count);
        sio_log_info("  空洞占比 : %.2f%% （尚未写入任何数据）",
                     sio_hole_ratio(&st) * 100.0);
    }
    return SIO_OK;
}
