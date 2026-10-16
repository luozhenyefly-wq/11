# sparseio —— 基于文件 IO 的空洞文件「断点续传」下载器

> 编程作业（AI 协作版）
> 平台：**Ubuntu 22.04.5 LTS (x86_64)**，工具链 `gcc 11.4 + GNU make`
> 语言：C11（仅 POSIX / libc，无第三方依赖）
> 说明：源文件与 README 由 AI（DeepSeek）协作产出，协作过程与采纳/拒绝判断见 `AI协作日志.md`。

---

## 1. 这个程序解决什么问题

普通下载器把文件当成「一整块连续空间」：要么一次写完，要么从头再来。
本程序把目标文件做成**稀疏文件（sparse file）**，再对每个数据块做**定点写入 + 进度位图记账**，
于是具备两个能力：

| 能力 | 实现手段 |
| --- | --- |
| 还没下载的部分**不占磁盘空间** | `ftruncate()` 撑开逻辑大小，被跳过的区间成为空洞，读出来是 `0x00`，`du` 统计为 0 |
| 中断后**从断点续传**，不重下已完成块 | 每块写完并回读校验后，把 bit 置位写进 sidecar 位图文件 `<target>.sio`，重启时按位图跳过 |

一句话：**文件逻辑上是 128KB/8MB/…，磁盘上只占你真正下过的那几块。**

---

## 2. 功能要求对照（作业 ①）

| 要求 | 命令 | 行为 |
| --- | --- | --- |
| **create**：生成空洞文件，逻辑 ≥128KB，未写入区不占磁盘块 | `sparseio create` | `open(O_CREAT)` + `ftruncate(size)`；`ftruncate` 不可用时回退 `lseek(size-1)` + 写 1 字节 + 截断，并告警 |
| **write**：`lseek`+`write` 定点写入，不破坏其余空洞 | `sparseio write` | 逐块 `pwrite(fd, buf, len, off)`，只碰 `[off, off+len)`；写完立刻 `pread` 回读比对，通过才记账 |
| **续传**：记录「已完成偏移」，Ctrl-C 后从断点继续 | `sparseio write`（重跑） | 每块提交后 `fsync(file)` → 位图置位 → 原子 `rename` 落盘；`SIGINT/SIGTERM` 安全退出，退出码 130 |
| **status**：打印逻辑大小 / 实际占块数 / 空洞占比 | `sparseio status` | 读 `stat()` 的 `st_size` 与 `st_blocks`，算空洞占比与分块进度；`--json` 便于脚本断言 |

额外提供 **verify**：逐块回读，已写块与数据源比对，未写块必须是 `0x00`（证明空洞没被破坏）。

---

## 3. 编译与运行

```bash
# 编译（默认 -O2 -Wall -Wextra，零告警）
make

# 调试版：AddressSanitizer + UBSan
make clean && make DEBUG=1

# 全部测试用例（空洞/定点写入/断点续传/边界/信号）
make test

# 断点续传演示（含本地 HTTP Range 数据源）
make demo

# 一键生成实验报告证据（logs/ 目录）
bash report.sh
```

### 命令行

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
  --verbose / --json
```

### 最小可用示例

```bash
# 1) 想下一个 8MiB 的文件：先建空洞
sparseio create --target=demo.bin --size=8M --block=256K
du -h demo.bin            # 0 字节 —— 全是空洞

# 2) 下载 5 块后中断（模拟网络抖动 / Ctrl-C）
sparseio write --target=demo.bin --source=file:///tmp/src.bin --block=256K --limit=5

# 3) 看状态：逻辑 8MiB，实际只占 1.25MiB，空洞占比 ~84%
sparseio status --target=demo.bin

# 4) 续传：从断点继续，已完成块被跳过
sparseio write --target=demo.bin --source=file:///tmp/src.bin --block=256K
```

---

## 4. 关键实现

### 4.1 空洞是怎么来的（`src/sio_sparse.c`）

```c
fd = open(path, O_RDWR | O_CREAT | (force ? O_TRUNC : O_EXCL), 0644);
ftruncate(fd, size);      /* 只改 i_size，不分配数据块 -> ext4 上全是空洞 */
```

`ftruncate()` 只更新 inode 的长度字段，**不写任何数据块**。此后：
- `stat().st_size` = `size`（逻辑大小）
- `stat().st_blocks * 512` = 0（实际占用）
- 读空洞区间得到全 `0x00`（内核按需返回零页，这也是 `pread` 在空洞上返回 0 填充的原因）

回退路径：某些文件系统/限制下 `ftruncate` 可能失败，此时用 `lseek(fd, size-1, SEEK_SET)` 写 1 字节再截断，
中间区间同样是空洞（代价是多占 1 个数据块），程序会 `WARN` 明确告知。

### 4.2 定点写入与「不破坏其余空洞」（`src/sio_main.c`）

```c
sio_map_block_range(&m, i, &off, &len);          /* 算出本块的偏移与长度 */
sio_source_pread(src, wbuf, len, off);           /* 只取这一段 */
sio_pwrite_full(fd, wbuf, len, off);             /* pwrite：定点写，不产生顺序 IO */
sio_pread_full(fd, rbuf, len, off);              /* 立刻回读校验 */
if (memcmp(rbuf, wbuf, len) == 0) sio_map_set(&m, i, 1);
```

`pwrite` 自带偏移参数，**不需要改文件偏移量**，因此：

- 不会碰到其它块；
- 不会把空洞「顺带写实」；
- 多线程/多进程同时写不同块也不会互相错位（本次为单线程，但语义上安全）。

### 4.3 断点续传的进度存档（`src/sio_bitmap.c`）

位图文件 `<target>.sio` 布局：

```
[ sio_map_hdr ][ ceil(nblocks/8) 字节位图 ]
   magic "SIOBITMAP\1" | version | block_size | nblocks
   total_size | head_size | src_size | src_uri | src_sha256
```

提交顺序（每一步都保证「位图不撒谎」）：

```
pwrite 数据 → pread 回读比对 → fsync(目标文件) → 位图置位 → 写 .tmp → fsync → rename 覆盖
```

- `rename` 是原子的：位图要么是旧的完整版本，要么是新的完整版本，**不会出现半个位图**；
- `fsync` 在置位之前：即使此刻掉电，也只是「数据已写但位图没记」，下次重下这一块，**不会漏数据**；
- 位图直接记录 `block_size / total_size / head_size`，一旦与本次命令参数不一致就报错退出（`exit 5`），
  避免「用不同分块策略静默重下」把已写数据覆盖成错位内容。

### 4.4 SIGINT / Ctrl-C 安全退出

```c
sa.sa_flags = 0;          /* 不加 SA_RESTART，让阻塞的 read/write 也能被打断 */
```

信号处理函数只做一件事：置 `sig_atomic_t` 标志位。
主循环在**块边界**检查标志，因此退出永远发生在「一块已经完整提交」之后，
不会留下写了一半的块，也不会丢进度。退出码统一为 `130`。

### 4.5 逻辑大小校验

`create` 之后立即 `stat` 复核 `st_size`，`write` 前后复核目标大小，
并拒绝「目标大小 > 源大小」的请求（`exit 4`），避免把文件尾部撑出一段永远填不上的垃圾。

### 4.6 数据源抽象（`src/sio_http.c`）

```c
typedef struct sio_source sio_source;                 /* 不透明类型，调用方看不到实现 */
int     sio_source_open(sio_source **out, const char *url);
ssize_t sio_source_pread(sio_source *s, void *buf, size_t len, uint64_t off);
```

- `file://` 源：`open` + `pread`，用于无网络的自动化测试与结果复现；
- `http(s)://` 源：每个块发一次 `Range: bytes=a-b`，期望 `206 Partial Content`，
  并校验返回的 `Content-Range` 起点与请求一致；服务端忽略 `Range` 返回 `200` 时，流式跳过前面字节（退化为顺序流）；
- 长度探测：`Range: bytes=0-0` 拿 `Content-Range: bytes 0-0/total` 里的 total。

---

## 5. 测试

### 5.1 用例清单（`tests/test_all.sh`）

| 编号 | 用例 | 断言 |
| --- | --- | --- |
| T1 | 空洞创建 | 逻辑大小 = 8MiB；实际占用 < 64KiB；位图生成 |
| T2 | 定点写入 + 中断 | 退出码 130；恰好提交 7 块；已写区间与源逐字节一致；未写区间非零字节数 = 0；磁盘占用只随已写块增长 |
| T3 | 断点续传 | 重跑后退出码 0；跳过 7 个已完成块；只补写剩余字节 |
| T4 | 数据一致性 | 目标 `sha256` = 源 `sha256`；`verify` 逐块通过 |
| T5 | 边界 | 下限 128KB 可建；64KB 被拒（exit 2）；非块对齐 + 尾块正确；目标 > 源被拒（exit 4）；重复续传幂等；参数不一致被拒（exit 5）；缺参数 exit 2 |
| T6 | `SIGINT` | 退出码 130；中断前进度已落盘；续传后 sha256 一致 |

### 5.2 运行结果

> **这一节由运行环境决定，请在目标机器上执行一次后填写，不要照抄。**
>
> 生成方式（在 Ubuntu 22.04.5 里、工程根目录执行）：
>
> ```bash
> bash report.sh     # 编译 + 全部测试 + 演示，输出留档到 logs/
> ```
>
> 跑完后把下面两处补齐即可：
>
> 1. `logs/02_test.log` 末尾的汇总行，例如
>    `用例通过: NN` / `用例失败: 0` / `ALL TESTS PASSED`；
> 2. `logs/03_demo.log` 里「完成后实际占用 / 逻辑大小」那一对数字
>    （用来证明空洞最终被数据填满、sha256 与源一致）。
>
> | 证据 | 位置 | 内容 |
> | --- | --- | --- |
> | 编译告警 | `logs/01_build.log` | `-Wall -Wextra` 下应无 warning |
> | 测试汇总 | `logs/02_test.log` | T1–T6 全部 PASS |
> | 断点续传 | `logs/03_demo.log` | 「已完成偏移 [off, +len)」「跳过 N 块」 |
> | HTTP Range | `logs/04_http_range.log` | `curl -D -` 返回 `206 Partial Content` |
> | 运行截图 | `screenshots/` | 见 `截图清单.md` 的 6 条命令 |
>
> 未跑之前，本节的实测数字**留空**，不以推测值代替——这也是本次 AI 协作的约定之一
> （见 `AI协作日志.md` 第 6 节：文档里凡是「没有实测支撑」的说法一律删掉）。

### 5.3 怎么自己复现

```bash
make clean && make && make test      # 全部用例
bash tests/demo.sh                   # 断点续传演示
bash report.sh                       # 生成全部证据日志
```

---

## 6. 目录结构

```
sparseio/
├── Makefile                编译/测试/打包（-Wall -Wextra 零告警，支持 DEBUG=1 走 ASan+UBSan）
├── README.md               本文件：功能对照、实现原理、编译运行、测试清单
├── AI协作日志.md            AI 协作全记录：9 条 prompt、采纳/部分采纳/拒绝判断、12 条代码审查缺陷
├── AI辅助测试.md            AI 辅助测试记录：测试策略、可判定命题、用例设计理由、S1–S7 脚本缺陷、局限
├── 编译与运行说明.md         在 Ubuntu 22.04.5 里怎么编、怎么跑、退出码约定、常见问题
├── 截图清单.md              需要交的运行截图：命令、预期画面、命名（照着敲即可）
├── 提交清单.md              交作业前逐项勾选（文件齐不齐、证据有没有）
├── report.sh               一键跑完编译+测试+演示+HTTP 冒烟，证据留档到 logs/
├── src/
│   ├── sio_common.h        公共类型、常量、错误码、函数声明
│   ├── sio_main.c          命令行入口：create / write / status / verify
│   ├── sio_sparse.c        稀疏文件创建与 st_blocks 统计
│   ├── sio_bitmap.c        进度位图（断点续传的进度存档）
│   ├── sio_http.c          数据源：file:// 与 http(s)://；HTTP 侧有 curl 通道与内置 socket 通道两条实现
│   ├── sio_sha256.c        自包含 SHA-256（不依赖 OpenSSL）
│   └── sio_util.c          日志、参数解析、信号、pread/pwrite 封装
├── tests/
│   ├── test_all.sh         6 组测试用例（T1–T6，29 条断言）
│   ├── demo.sh             演示脚本（起自写的 206 服务端，走真实 Range 续传）
│   ├── range_server.py     自写的支持 Range 的最小 HTTP 服务端（用于 demo 与 HTTP 用例）
│   └── work/               测试产物，含 work/test_all.log（运行后生成）
├── logs/                   证据日志：01_build / 02_test / 03_demo / 04_http_range（运行后生成）
└── screenshots/            运行截图（按 截图清单.md 补入）
```
├── logs/                   实验报告证据日志：01_build / 02_test / 03_demo / 04_http_range（运行后生成）
└── screenshots/            运行截图（按 截图清单.md 补入）
```

---

## 7. 已知限制

1. `status` 的「实际占用」来自 `st_blocks`，其粒度是文件系统块（ext4 为 4KiB）；
   写入 1 字节也会体现为 1 个块（4KiB）的占用，因此空洞占比是**下限估计**，不是精确字节级统计。
2. 位图按块记账，块大小一旦确定就不可更改（程序会拒绝，而不是静默重下）。
3. HTTP 源依赖服务端支持 `Range`；不支持时退化为顺序流（功能正确但失去「只取需要的段」的优势）。
4. 单线程顺序下载；未做多连接分块并发（作业未要求，且会让位图提交时序复杂化）。
5. 稀疏行为按 ext4 语义实现并验证（`ftruncate` 只改 `i_size` 不分配数据块）；
   `tmpfs` 同样支持空洞，但 `btrfs`/`zfs` 的 `st_blocks` 语义不同，
   `status` 的数字需要按文件系统重新解读。
6. `write` 目前只有单线程顺序拉取；`--delay` 是给演示/中断用的模拟延迟，不是限速器。

---

## 8. 参考

- Linux `man 2 ftruncate` / `man 2 lseek` / `man 2 pwrite` / `man 2 stat`（`st_blocks` 语义）
- RFC 7233 *Hypertext Transfer Protocol (HTTP/1.1): Range Requests*
- FIPS 180-4 *Secure Hash Standard*（SHA-256）
