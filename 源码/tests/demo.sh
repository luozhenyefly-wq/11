#!/usr/bin/env bash
# ============================================================================
#  demo.sh - 一次可展示/可截图的完整演示
#    1) 起一个本地 HTTP 服务（Python http.server，支持 206 Range）
#    2) create 生成空洞文件，对比 逻辑大小 vs 实际占用
#    3) write 只下一部分 -> 演示「已完成偏移 [  x , + len )」与空洞
#    4) ls -ls / du 直接看到空洞
#    5) 续传完成 -> sha256 一致
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="$ROOT/sparseio"
WORK="$ROOT/tests/work/demo"
PORT=8099
URL_BASE="http://127.0.0.1:$PORT"

mkdir -p "$WORK"
rm -f "$WORK"/*

c_b=$'\033[1m'; c_y=$'\033[33m'; c_g=$'\033[32m'; c_r=$'\033[0m'
step() { echo; echo "${c_b}${c_y}========== $* ==========${c_r}"; }
run()  { echo "${c_g}\$ $*${c_r}"; "$@" || return $?; }

if [ ! -x "$BIN" ]; then echo "请先 make"; exit 1; fi

step "0. 准备：造一个 8MiB 的“服务器端文件”，用 HTTP Range 当数据源"
head -c $((8 * 1024 * 1024)) /dev/urandom > "$WORK/server_file.bin"
SRC_SHA=$(sha256sum "$WORK/server_file.bin" | awk '{print $1}')
ls -lh "$WORK/server_file.bin"
echo "源 sha256 = $SRC_SHA"

# 注意：python3 -m http.server（3.10）不支持 Range，任何 Range 请求都回 200+整文件，
# 下载器只能退化成“整文件流式跳过”，看不出断点请求的价值。
# 因此这里改用 tests/range_server.py —— 一个真正返回 206 Partial Content 的最小服务器。
# 用 setsid 起独立进程组，避免只 kill 掉包装 shell、python 仍占着端口不退出
# （残留进程会让下一次演示的 curl 仍然打到旧服务端上）。
setsid python3 "$ROOT/tests/range_server.py" "$WORK" "$PORT" >/dev/null 2>&1 &
HTTP_PID=$!
# 杀整个进程组：$HTTP_PID 是 setsid 后的组长 pid
trap 'kill -TERM -"$HTTP_PID" 2>/dev/null || kill $HTTP_PID 2>/dev/null' EXIT
sleep 1
echo "Range 数据源已启动: $URL_BASE/server_file.bin (pid=$HTTP_PID)"
echo "-- 用 curl 验证服务端确实支持 Range（应看到 206 + Content-Range）--"
curl -s -D - -o /dev/null -H 'Range: bytes=1000-1999' "$URL_BASE/server_file.bin" \
  | grep -iE '^HTTP/|Content-Range|Content-Length|Accept-Ranges' | sed 's/^/   /'

step "1. create：生成空洞文件，逻辑 8MiB，实际占用应为 0"
run "$BIN" create --target="$WORK/dl.bin" --source="$URL_BASE/server_file.bin" \
      --block=256K --head=64K
echo
echo "-- ls -ls 看第 1 列（占用块数）--"
ls -ls "$WORK/dl.bin"
echo "-- du 看真实磁盘占用 --"
du -h "$WORK/dl.bin"
echo "-- status --"
run "$BIN" status --target="$WORK/dl.bin" --block=256K --head=64K

step "2. write（只下 5 块就停）：观察「已完成偏移 [ off , + len )」"
run "$BIN" write --target="$WORK/dl.bin" --source="$URL_BASE/server_file.bin" \
      --block=256K --head=64K --limit=5 || true

step "3. 此刻的空洞：磁盘占用只涨了已下载的那几块"
run "$BIN" status --target="$WORK/dl.bin" --block=256K --head=64K
echo
echo "-- ls -ls：未下载部分仍然是稀疏的 --"
ls -ls "$WORK/dl.bin"

step "4. 断点续传：重跑同一条 write，从断点继续（不从头下）"
run "$BIN" write --target="$WORK/dl.bin" --source="$URL_BASE/server_file.bin" \
      --block=256K --head=64K

step "5. 校验：逐块回读 + sha256 对比"
run "$BIN" verify --target="$WORK/dl.bin" --source="$URL_BASE/server_file.bin" \
      --block=256K --head=64K
echo
echo "源   sha256 = $SRC_SHA"
echo "目标 sha256 = $(sha256sum "$WORK/dl.bin" | awk '{print $1}')"
echo
run "$BIN" status --target="$WORK/dl.bin" --block=256K --head=64K

step "演示结束"
echo "产物目录: $WORK"
echo "  dl.bin     下载结果（8MiB，已填满）"
echo "  dl.bin.sio 进度位图（断点续传的进度存档）"
