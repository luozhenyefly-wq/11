#!/usr/bin/env bash
# ============================================================================
#  空洞文件「断点续传」下载器 - 测试用例（AI 辅助测试的落地脚本）
#
#  覆盖：
#    T1 空洞创建      ：逻辑大小 >=128KB，未写入区不占磁盘块
#    T2 定点写入      ：只改目标区间，空洞字节保持 0x00
#    T3 断点续传      ：Ctrl-C/中断后重跑，从断点继续而不是从头开始
#    T4 位图持久化    ：进度文件在进程被杀后依然可用
#    T5 边界用例      ：最小 128KB / 非块对齐 / 尾块 / 重复续传 / 参数错误
#    T6 数据一致性    ：sha256 与源逐字节一致
#
#  用法： bash tests/test_all.sh          （在工程根目录执行）
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="$ROOT/sparseio"
WORK="$ROOT/tests/work"
LOG="$WORK/test_all.log"

PASS=0
FAIL=0
CASE_NO=0

mkdir -p "$WORK"
: > "$LOG"

c_reset=$'\033[0m'; c_red=$'\033[31m'; c_grn=$'\033[32m'
c_yel=$'\033[33m'; c_blu=$'\033[36m'; c_bold=$'\033[1m'

log()  { printf '%s\n' "$*" | tee -a "$LOG"; }
hr()   { log "$(printf '%.0s-' {1..72})"; }
title(){ hr; log "${c_bold}${c_blu}$*${c_reset}"; hr; }

ok()   { PASS=$((PASS+1)); log "  ${c_grn}[PASS]${c_reset} $*"; }
bad()  { FAIL=$((FAIL+1)); log "  ${c_red}[FAIL]${c_reset} $*"; }

# assert_eq <实际> <期望> <说明>
assert_eq() {
  if [ "$1" = "$2" ]; then ok "$3  ($1)"; else bad "$3  期望=$2 实际=$1"; fi
}
# assert_true <条件命令的退出码> <说明>
assert_true() {
  if [ "$1" -eq 0 ]; then ok "$2"; else bad "$2"; fi
}

# 实际占用磁盘字节数（st_blocks * 512）
alloc_bytes() { stat -c '%b * 512' "$1" | bc; }
logical_bytes() { stat -c '%s' "$1"; }

if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件 $BIN，请先执行 make" >&2
  exit 1
fi

title "环境信息"
log "  工程根目录 : $ROOT"
log "  可执行文件 : $BIN"
log "  内核       : $(uname -sr)"
log "  发行版     : $(. /etc/os-release; echo "$PRETTY_NAME")"
log "  架构       : $(uname -m)"
log "  文件系统   : $(df -T "$WORK" | awk 'NR==2{print $2}')  ($WORK)"
log "  gcc        : $(cc --version | head -1)"

# ---------------------------------------------------------------------------
title "准备测试数据"
SRC="$WORK/source.bin"
SRC_SIZE=$((4 * 1024 * 1024))
head -c "$SRC_SIZE" /dev/urandom > "$SRC"
SRC_SHA=$(sha256sum "$SRC" | awk '{print $1}')
log "  源文件     : $SRC ($SRC_SIZE 字节)"
log "  源 sha256  : $SRC_SHA"

TOTAL=$((4 * 1024 * 1024))     # 4 MiB >= 128 KiB
BLOCK=$((256 * 1024))          # 256 KiB/块 -> 头区 1 块 + 尾部 15 块 = 16 块
# 注：TOTAL 必须与 SRC_SIZE 一致。早期版本把 TOTAL 写成 6MiB 而源只有 4MiB，
# 下载器会（正确地）以“目标 > 源”拒绝写入（退出码 4），导致 T2~T4 连锁失败。
# 这属于测试设计缺陷而非被测程序缺陷，见报告“AI 代码审查 缺陷-S1”。
TGT="$WORK/target.bin"
MAP="$TGT.sio"
rm -f "$TGT" "$MAP"

# ---------------------------------------------------------------------------
title "T1 空洞创建：逻辑大小 >=128KB，未写入区不占磁盘块"
$BIN create --target="$TGT" --size="$TOTAL" --block="$BLOCK" --head=4096 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
assert_eq "$rc" 0 "create 退出码为 0"
assert_eq "$(logical_bytes "$TGT")" "$TOTAL" "逻辑大小为 $TOTAL 字节"
alloc=$(alloc_bytes "$TGT")
log "  实际占用   : $alloc 字节"
if [ "$alloc" -lt $((64 * 1024)) ]; then
  ok "未写入区不占磁盘块（实际占用 ${alloc}B << 逻辑 ${TOTAL}B）"
else
  bad "实际占用 ${alloc}B 偏大，稀疏性可能失效"
fi
assert_true $([ -f "$MAP" ] && echo 0 || echo 1) "进度位图已生成 $MAP"

# ---------------------------------------------------------------------------
title "T2 定点写入：只改目标区间，其余空洞保持 0x00"
$BIN write --target="$TGT" --source="file://$SRC" --block="$BLOCK" --head=4096 \
           --limit=7 --delay=0 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
assert_eq "$rc" 130 "被 --limit 主动中断时退出码为 130"
DONE_AFTER_INT=$($BIN status --target="$TGT" --block="$BLOCK" --head=4096 --json \
                  | grep blocks_done | tr -dc '0-9')
log "  中断时已完成块数: $DONE_AFTER_INT"
# --limit=7 的含义是“本次最多提交 7 个块”，第 0 块就是 4KiB 头区，故共 7 块。
assert_eq "$DONE_AFTER_INT" 7 "中断时恰好提交 7 个块（含头区块）"

log "  -- 校验已写区间与源一致 --"
# 已提交区间 = 头区 4KiB + 其后 6 个数据块
if cmp -n $((4096 + 6 * BLOCK)) -i 0:0 "$SRC" "$TGT"; then
  ok "已写入的连续区间与源逐字节一致"
else
  bad "已写入区间与源不一致"
fi

log "  -- 校验未写区间仍为空洞（全 0x00）--"
HOLE_OFF=$((4096 + 6 * BLOCK))
NONZERO=$(python3 - "$TGT" "$HOLE_OFF" "$((2 * BLOCK))" <<'PY'
import sys
path, off, ln = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(path, 'rb') as f:
    f.seek(off)
    data = f.read(ln)
print(sum(1 for b in data if b != 0))
PY
)
log "  未写区间非零字节数: $NONZERO（应为 0）"
assert_eq "$NONZERO" 0 "未写区间仍为 0x00（空洞未被破坏）"

log "  -- 未写区间的磁盘占用必须几乎为 0（真稀疏，不是写 0）--"
PART_ALLOC=$(alloc_bytes "$TGT")
log "  部分写入后实际占用: $PART_ALLOC 字节（已写 7 块共 $((4096 + 6*BLOCK)) 字节）"
if [ "$PART_ALLOC" -lt $((4096 + 8 * BLOCK)) ]; then
  ok "磁盘占用只随已写块增长，未写区没有分配块"
else
  bad "磁盘占用 ${PART_ALLOC}B 超过已写数据量，稀疏性异常"
fi

# ---------------------------------------------------------------------------
title "T3 断点续传：重跑 write，从断点继续"
# 先只补 1 个块，让文件保持“未完成”状态，这样才能观察“跳过已完成块”。
# （缺陷-S6：原脚本先跑完整续传再做断言，那时文件已是 17/17，
#   只能得到“跳过 17 块”，测不到“复用断点、只补剩余”的语义。）
log "  -- 续传是否真的跳过了已完成块（--limit=1，只补 1 块）--"
# 注：下载器日志走 stderr（sio_util.c 的 vlog），必须 2>&1，
# 否则 RESUME_OUT 为空、解析出 0，误判成“没跳过任何块”（缺陷-S5）。
# --limit 生效时会返回 130，这里加 || true 避免影响脚本流程。
RESUME_OUT=$($BIN write --target="$TGT" --source="file://$SRC" --block="$BLOCK" --head=4096 \
                    --limit=1 2>&1 || true)
printf '%s\n' "$RESUME_OUT" | tee -a "$LOG"
SKIPPED=$(printf '%s\n' "$RESUME_OUT" | grep -o '本次跳过（已完成）[0-9]*' | tr -dc '0-9')
NEWBYTES=$(printf '%s\n' "$RESUME_OUT" | grep -o '本次新写入 [0-9]*' | tr -dc '0-9')
log "  本次跳过块数: ${SKIPPED:-0}   本次新写入字节: ${NEWBYTES:-0}"
assert_eq "${SKIPPED:-0}" "$DONE_AFTER_INT" "续传复用了位图，跳过 $DONE_AFTER_INT 个已完成块"
assert_eq "${NEWBYTES:-0}" "$BLOCK" "续传只补写了 1 个块（$BLOCK 字节）"

log "  -- 续传到完成 --"
$BIN write --target="$TGT" --source="file://$SRC" --block="$BLOCK" --head=4096 2>&1 | tail -1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
assert_eq "$rc" 0 "续传后退出码为 0"

# ---------------------------------------------------------------------------
title "T4 数据一致性：sha256 与源一致"
DST_SHA=$(sha256sum "$TGT" | awk '{print $1}')
log "  源 sha256  : $SRC_SHA"
log "  目标 sha256: $DST_SHA"
assert_eq "$DST_SHA" "$SRC_SHA" "目标文件 sha256 与源完全一致"

$BIN verify --target="$TGT" --source="file://$SRC" --block="$BLOCK" --head=4096 2>&1 | tee -a "$LOG"
assert_eq "${PIPESTATUS[0]}" 0 "verify 逐块校验通过"

$BIN status --target="$TGT" --block="$BLOCK" --head=4096 2>&1 | tee -a "$LOG"
FINAL_ALLOC=$(alloc_bytes "$TGT")
log "  完成后实际占用: $FINAL_ALLOC / $TOTAL 字节（应接近 TOTAL，空洞已被数据填满）"

# ---------------------------------------------------------------------------
title "T5 边界用例"
log "-- T5.1 作业下限 128KB --"
T51="$WORK/min128k.bin"
SRC51="$WORK/small128k.bin"
head -c 131072 /dev/urandom > "$SRC51"
rm -f "$T51" "$T51.sio"
# 注：块大小必须 <= 总大小，早期这里用 --block=1M 配 128K 源，
# 会被下载器的参数校验正确拒绝（退出码 2），属于测试设计缺陷（缺陷-S2）。
$BIN create --target="$T51" --source="file://$SRC51" --block=64K --head=0 2>&1 | tail -4 | tee -a "$LOG"
assert_eq "$(logical_bytes "$T51")" 131072 "逻辑大小=131072（源推导）"
$BIN write --target="$T51" --source="file://$SRC51" --block=64K --head=0 >/dev/null 2>&1
assert_eq "$?" 0 "128KB 源可完整下载"
assert_eq "$(sha256sum "$T51" | awk '{print $1}')" \
          "$(sha256sum "$SRC51" | awk '{print $1}')" "128KB 数据一致"

log "-- T5.2 小于 128KB 必须被拒绝 --"
$BIN create --target="$WORK/toosmall.bin" --size=65536 --force >/dev/null 2>&1
assert_eq "$?" 2 "逻辑大小 64KB 被拒绝（退出码 2）"

log "-- T5.3 非块对齐 + 尾块 --"
T53="$WORK/unaligned.bin"
SRC53="$WORK/src_unaligned.bin"
head -c $((128 * 1024 + 12345)) /dev/urandom > "$SRC53"
rm -f "$T53" "$T53.sio"
$BIN create --target="$T53" --source="file://$SRC53" --block=64K --head=100 2>&1 | tail -3 | tee -a "$LOG"
$BIN write --target="$T53" --source="file://$SRC53" --block=64K --head=100 >/dev/null 2>&1
assert_eq "$?" 0 "非对齐大小续传完成"
assert_eq "$(sha256sum "$T53" | awk '{print $1}')" \
          "$(sha256sum "$SRC53" | awk '{print $1}')" \
          "非对齐目标 sha256 一致"

log "-- T5.4 目标大于源必须报错 --"
T54="$WORK/big.bin"
SRC54="$WORK/small_src.bin"
head -c $((1 * 1024 * 1024)) /dev/urandom > "$SRC54"
rm -f "$T54" "$T54.sio"
$BIN create --target="$T54" --size=2M --block=64K --force >/dev/null 2>&1
$BIN write --target="$T54" --source="file://$SRC54" --block=64K >/dev/null 2>&1
assert_eq "$?" 4 "目标(2M) > 源(1M) 时退出码 4"

log "-- T5.5 重复续传幂等（已完成后再次 write）--"
$BIN write --target="$TGT" --source="file://$SRC" --block="$BLOCK" --head=4096 \
           --verbose 2>&1 | grep -q "本次新写入 0 字节"
assert_eq "$?" 0 "文件已完成时本次新写入 0 字节（幂等）"

log "-- T5.6 参数一致性保护：改块大小必须拒绝 --"
$BIN write --target="$TGT" --source="file://$SRC" --block=128K --head=4096 >/dev/null 2>&1
assert_eq "$?" 5 "块大小与位图不一致时退出码 5（拒绝静默重下）"

log "-- T5.7 缺少必填参数必须报用法错误 --"
$BIN write --target="$TGT" >/dev/null 2>&1
assert_eq "$?" 2 "write 缺 --source 时退出码 2"
$BIN badcmd >/dev/null 2>&1
assert_eq "$?" 2 "未知子命令退出码 2"

# ---------------------------------------------------------------------------
title "T6 截断/中断信号（模拟 Ctrl-C）"
T6="$WORK/sigint.bin"
rm -f "$T6" "$T6.sio"
$BIN create --target="$T6" --source="file://$SRC" --block=256K --head=4096 >/dev/null 2>&1
# 注：早期写法是 `wait $WPID 2>/dev/null; rc=$?`，wait 一旦失败会让整个脚本
# 在 make 下报 “wait: 没有子进程” 并中断 T6 段（测试脚本缺陷-S4）。
# 这里改成显式捕获退出码，且把它包在 if 里，避免 set -e / make 误判。
# --delay 必须足够大，保证 kill 发出时进程还在下载中（17 块 * 300ms ≈ 5.1s > 1.5s）。
# 早期用 --delay=60，块数变多后会在 1.5s 内跑完，SIGINT 打到已正常退出的进程上，
# 于是拿到退出码 0 而误判（测试脚本缺陷-S7）。
setsid $BIN write --target="$T6" --source="file://$SRC" --block=256K --head=4096 \
       --delay=300 > /dev/null 2>&1 &
WPID=$!
sleep 1.5
kill -INT -"$(ps -o pgid= -p "$WPID" | tr -d ' ')" 2>/dev/null || kill -INT "$WPID" 2>/dev/null
rc=0
if wait "$WPID"; then rc=0; else rc=$?; fi
log "  SIGINT 后退出码: $rc"
assert_eq "$rc" 130 "SIGINT 中断退出码 130"
PARTIAL=$($BIN status --target="$T6" --block=256K --head=4096 --json | grep blocks_done | tr -dc '0-9')
log "  中断时已完成块数: $PARTIAL"
[ "$PARTIAL" -gt 0 ] && ok "中断前已提交 $PARTIAL 个块（进度已落盘）" || bad "进度丢失"
$BIN write --target="$T6" --source="file://$SRC" --block=256K --head=4096 >/dev/null 2>&1
assert_eq "$?" 0 "SIGINT 之后可以续传完成"
assert_eq "$(sha256sum "$T6" | awk '{print $1}')" "$SRC_SHA" "SIGINT 续传后数据一致"

# ---------------------------------------------------------------------------
title "测试结果汇总"
log "  用例通过: ${c_grn}$PASS${c_reset}"
log "  用例失败: ${c_red}$FAIL${c_reset}"
log "  完整日志: $LOG"
if [ "$FAIL" -eq 0 ]; then
  log "${c_grn}${c_bold}ALL TESTS PASSED${c_reset}"
  exit 0
else
  log "${c_red}${c_bold}THERE ARE FAILURES${c_reset}"
  exit 1
fi
