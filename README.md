# 编程作业（AI 协作版）— 基于文件 IO 的空洞文件「断点续传」下载器

本仓库是这份作业的完整交付：程序源码、编译脚本、测试用例、AI 协作记录与真实运行日志。

**运行环境**：Ubuntu 22.04.5 LTS（VMware 虚拟机），内核 `6.8.0-138-generic`，gcc 11.4 + GNU make，ext4 文件系统
**实现语言**：C11，仅依赖 POSIX / libc，无第三方库

---

## 实测结果

| 项目 | 结果 |
| --- | --- |
| 编译 `make clean && make` | **0 warning / 0 error** |
| 测试 `make test` | 退出码 0，**29 条断言全部通过**，输出 `ALL TESTS PASSED` |
| HTTP Range 端到端（真实 206 数据源，4 MiB） | 中断 → 续传 → 校验，**sha256 完全一致** |
| 空洞效果 | 逻辑 4.00 MiB / 实际 832 KiB / **空洞占比 79.69%**，`du` 显示 0 |

---

## 目录结构

```
.
├── 交付说明.md                   总说明：要求对照、实测结果、复现命令、已知限制
├── 源码/
│   ├── Makefile                  编译 / 测试 / 演示 / 打包 / 调试版(ASan+UBSan)
│   ├── README.md                 设计说明：功能对照、关键实现、已知限制
│   ├── AI协作日志.md              9 条 prompt + 采纳/拒绝判断 + 12 处缺陷审查记录
│   ├── AI辅助测试.md              29 条用例清单表 + 边界设计 + 测试局限
│   ├── 编译与运行说明.md           命令速查
│   ├── 提交清单.md                对照作业要求逐条自查
│   ├── 截图清单.md                需要截哪几张图、对应命令
│   ├── 虚拟机操作说明.md
│   ├── src/                      sio_main.c / sio_sparse.c / sio_bitmap.c
│   │                             sio_http.c / sio_sha256.c / sio_util.c / sio_common.h
│   ├── tests/                    test_all.sh（29 断言）· demo.sh · range_server.py
│   └── logs/                     运行日志证据
└── 运行记录/                      虚拟机终端导出的真实输出
    ├── 01_build.log
    ├── 02_test.log
    ├── 03_demo_http_range.log
    └── 04_http_range_e2e.log     HTTP Range 端到端完整记录（含 sha256 校验）
```

---

## 四项功能要求

| 要求 | 命令 | 实现要点 |
| --- | --- | --- |
| **create** 生成空洞文件（逻辑 ≥128KB，未写入区不占磁盘块） | `sparseio create` | `ftruncate()` 只撑逻辑大小、不落数据块；失败回退 `lseek(size-1)` + 写 1 字节再截断 |
| **write** `lseek`+`write` 定点写入，不破坏其余空洞 | `sparseio write` | 逐块 `pwrite(fd,buf,len,off)`，写完立刻 `pread` 回读 `memcmp` 校验，通过才记账 |
| **续传** 记录已完成偏移，Ctrl-C 后从断点继续 | `sparseio write`（重跑） | `pwrite 数据 → pread 比对 → fsync(目标) → 位图置位 → 写 .tmp → fsync → rename`；中断退出码 130 |
| **status** 打印逻辑大小 / 实际占块数 / 空洞占比 | `sparseio status` | 读 `stat()` 的 `st_size` 与 `st_blocks`（×512），算空洞占比与分块进度，支持 `--json` |

额外提供 **verify**：逐块回读，已写块与源比对、未写块必须是 `0x00`（证明空洞没被破坏）。

---

## 复现

```bash
cd 源码
make clean && make      # 编译：应 0 warning
make test               # 29 条断言，最后一行 ALL TESTS PASSED
make demo               # 断点续传演示（自动起支持 206 的本地 Range 数据源）
bash report.sh          # 一键生成全部证据日志到 logs/
```

端到端手工版（对应 `运行记录/04_http_range_e2e.log`）：

```bash
W=/tmp/show; rm -rf $W; mkdir -p $W
head -c 4194304 /dev/urandom > $W/s.bin
python3 tests/range_server.py $W 8099 &

cd $W; B=/path/to/sparseio
$B create --target=dl.bin --source=http://127.0.0.1:8099/s.bin --block=256K --head=64K
ls -ls dl.bin; du -h dl.bin                    # 逻辑 4MiB，占用 0
$B write  --target=dl.bin --source=http://127.0.0.1:8099/s.bin --block=256K --head=64K --limit=4
$B status --target=dl.bin --block=256K --head=64K   # 空洞占比 ~79%
$B write  --target=dl.bin --source=http://127.0.0.1:8099/s.bin --block=256K --head=64K   # 续传
$B verify --target=dl.bin --source=http://127.0.0.1:8099/s.bin --block=256K --head=64K
sha256sum s.bin dl.bin                         # 两者一致
```

---

## 命令行

```
sparseio <create|write|status|verify> [选项]

  --target=PATH     目标文件（默认 out.bin）
  --source=URI      数据源：file:///abs/path 或 http://host[:port]/path
  --block=SIZE      每块字节数（默认 64K）
  --head=SIZE       前缀连续区大小（默认 4K）
  --size=SIZE       目标逻辑大小；create 不填时取源文件大小
  --bitmap=PATH     进度位图（默认 <target>.sio）
  --limit=N         write 最多提交 N 个块后主动退出（演示中断用）
  --delay=MS        每块写完后的模拟延迟，便于观察/中断
  --retry=N         单块失败重试次数（默认 3）
  --verbose / --json / --force / -h
```

退出码：`0` 成功 · `2` 参数错误 · `3` IO 错误 · `4` 区间非法 · `5` 位图参数不一致 · `130` 被中断（可续传）

---

## 原理速览

1. **空洞怎么来**：`ftruncate()` 只更新 inode 长度字段、不分配数据块 → `st_size` 是 4 MiB、`st_blocks×512` 是 0，读空洞得到全 `0x00`，`du` 显示 0。
2. **定点写入为何不破坏其它空洞**：用 `pwrite` 而非 `write`，自带偏移、不改文件偏移量，只碰 `[off, off+len)`。
3. **续传靠什么记账**：sidecar 位图 `<target>.sio`（magic + 版本 + 块大小 + 块数 + 总大小 + 源信息 + `ceil(nblocks/8)` 字节位图）。**先 `fsync` 数据、再置位、再原子 `rename`**，保证「位图不撒谎」：掉电最多重下一块，绝不漏数据；位图记录分块参数，对不上就 `exit 5` 拒绝静默重下。

---

## 已知限制

1. `status` 的「实际占用」来自 `st_blocks`，粒度是文件系统块（ext4 为 4 KiB），写入 1 字节也计 1 块，故空洞占比是**下限估计**。
2. 位图按块记账，块大小一经确定不可更改（程序会拒绝）。
3. HTTP 源优先用 `curl -r a-b` 拉段；`curl` 不可用时回退内置 socket 实现。服务端不支持 Range（返回 200）时退化为整文件流式跳过。
4. 单线程顺序下载，未做多连接并发（作业未要求，且会让位图提交时序复杂化）。
5. 仅在 ext4 上实测稀疏行为；`btrfs`/`zfs` 的 `st_blocks` 语义不同。
6. Python 3.10 自带的 `http.server` **不支持 Range**（Range 请求回 200 + 整文件），故演示与测试改用 `tests/range_server.py`（正确返回 206）。
