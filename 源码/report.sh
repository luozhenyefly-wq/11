#!/usr/bin/env bash
# ============================================================================
#  实验报告生成脚本（AI 代码审查 / 交付截图取证用）
#
#  在 Ubuntu 22.04.5 虚拟机内、工程根目录执行：
#      bash report.sh
#
#  它会：
#    1) make 编译（并把编译告警一并留档）
#    2) make test 跑全部测试用例
#    3) make demo 跑断点续传演示
#    4) 冒烟测试真实 HTTP Range（python3 -m http.server 支持 206）
#    5) 把上面全部输出写进 logs/ 目录，并打印每个证据文件的行数与 sha256
# ============================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

LOGDIR="$ROOT/logs"
mkdir -p "$LOGDIR"

c_b=$'\033[1m'; c_y=$'\033[33m'; c_g=$'\033[32m'; c_r=$'\033[0m'
step() { echo; echo "${c_b}${c_y}========== $* ==========${c_r}"; }

echo "${c_b}环境${c_r}"
uname -a
. /etc/os-release && echo "发行版: $PRETTY_NAME"
echo "gcc: $(cc --version | head -1)"
echo "磁盘: $(df -hT "$ROOT" | tail -1)"

step "1/4 make clean && make（编译并留档告警）"
make clean  >"$LOGDIR/01_build.log" 2>&1
make        >>"$LOGDIR/01_build.log" 2>&1
BUILD_RC=$?
echo "退出码: $BUILD_RC"
tail -20 "$LOGDIR/01_build.log"
if [ "$BUILD_RC" -ne 0 ]; then
  echo "${c_g}编译失败，先修编译错误${c_r}"
  exit 1
fi

step "2/4 make test（全部测试用例）"
make test >"$LOGDIR/02_test.log" 2>&1
TEST_RC=$?
tail -25 "$LOGDIR/02_test.log"
echo "退出码: $TEST_RC"

step "3/4 make demo（断点续传演示）"
make demo >"$LOGDIR/03_demo.log" 2>&1
DEMO_RC=$?
echo "退出码: $DEMO_RC（末尾摘要）"
tail -15 "$LOGDIR/03_demo.log"

step "4/4 真实 HTTP Range 冒烟测试"
SMOKE="$LOGDIR/04_http_range.log"
{
  echo "### 目的：证明 write 走的是标准 HTTP Range(206) 请求"
  echo
  WORK="$ROOT/tests/work/smoke"
  rm -rf "$WORK"; mkdir -p "$WORK"
  head -c $((4 * 1024 * 1024)) /dev/urandom > "$WORK/remote.bin"
  ( cd "$WORK" && python3 -m http.server 8097 --bind 127.0.0.1 >/dev/null 2>&1 ) &
  HPID=$!
  sleep 1
  echo "--- 源 ---"
  ls -l "$WORK/remote.bin"
  echo
  echo "--- curl 直接验证服务端支持 Range（应返回 206）---"
  curl -s -o /dev/null -D - -H 'Range: bytes=1000-1999' \
       'http://127.0.0.1:8097/remote.bin' | head -8
  echo
  echo "--- sparseio create + write（HTTP 源，先只下 3 块）---"
  "$ROOT/sparseio" create --target="$WORK/http.bin" \
        --source="http://127.0.0.1:8097/remote.bin" --block=512K --head=131072
  "$ROOT/sparseio" write --target="$WORK/http.bin" \
        --source="http://127.0.0.1:8097/remote.bin" --block=512K --head=131072 \
        --limit=3
  echo "中断退出码: $?（130 = 主动中断）"
  echo
  echo "--- status（此时应有明显空洞）---"
  "$ROOT/sparseio" status --target="$WORK/http.bin" --block=512K --head=131072
  echo
  echo "--- 续传完成 ---"
  "$ROOT/sparseio" write --target="$WORK/http.bin" \
        --source="http://127.0.0.1:8097/remote.bin" --block=512K --head=131072
  echo
  echo "--- 校验 ---"
  "$ROOT/sparseio" verify --target="$WORK/http.bin" \
        --source="http://127.0.0.1:8097/remote.bin" --block=512K --head=131072
  echo
  echo "--- sha256 对比 ---"
  echo "源   : $(sha256sum "$WORK/remote.bin" | awk '{print $1}')"
  echo "目标 : $(sha256sum "$WORK/http.bin"  | awk '{print $1}')"
  kill $HPID 2>/dev/null
} >"$SMOKE" 2>&1
echo "退出码: $?"
tail -20 "$SMOKE"

step "证据清单"
for f in "$LOGDIR"/*.log; do
  printf '  %-28s %6s 行  sha256=%s\n' "$(basename "$f")" "$(wc -l < "$f")" \
         "$(sha256sum "$f" | cut -c1-16)"
done

echo
echo "${c_b}结果${c_r}: 构建=$BUILD_RC 测试=$TEST_RC 演示=$DEMO_RC"
if [ "$TEST_RC" -eq 0 ]; then
  echo "${c_g}测试全部通过${c_r}"
else
  echo "测试存在失败，详见 $LOGDIR/02_test.log"
fi
echo "全部日志目录: $LOGDIR"
