#!/bin/bash
# run_all_tests.sh — 跨仓库一键全测试（mkpms + rustFrida host 侧）。
#
# 覆盖：
#   mkpms:      ctest 19 套件（host 模式，KPM 模块自动跳过）
#               kernel_syntax_check（14 内核源文件，需 KernelPatch 源码）
#   rustFrida:  host-tests（10 测试）+ trace-decoder（8 测试）
#
# 用法：
#   ./scripts/run_all_tests.sh [KP_ROOT]
#   KP_ROOT 默认 /mnt/data/Work/Projects/KernelPatch（本地源码副本）
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
KP_ROOT="${1:-/mnt/data/Work/Projects/KernelPatch}"
RF="$(dirname "$HERE")/rustFrida"

FAIL=0

echo "=== [1/4] mkpms build + ctest (host) ==="
rm -rf "$HERE/build-test"
cmake -S "$HERE" -B "$HERE/build-test" -DKP_DIR="$HERE" >/dev/null
cmake --build "$HERE/build-test" >/dev/null
if ctest --test-dir "$HERE/build-test" >/dev/null; then
    echo "  ok: mkpms ctest $(ctest --test-dir "$HERE/build-test" -N 2>/dev/null | grep -oP 'Total Tests: \d+' || echo 'all')"
else
    echo "  FAIL: mkpms ctest"; FAIL=1
fi

echo "=== [2/4] kernel syntax check ==="
if "$HERE/scripts/kernel_syntax_check.sh" "$KP_ROOT" >/dev/null; then
    echo "  ok: 14 kernel sources"
else
    echo "  FAIL: kernel syntax"; FAIL=1
fi

echo "=== [3/4] rustFrida host-tests ==="
if (cd "$RF/host-tests" && cargo test --quiet >/dev/null 2>&1); then
    echo "  ok: host-tests 10"
else
    echo "  FAIL: host-tests"; FAIL=1
fi

echo "=== [4/4] rustFrida trace-decoder ==="
if (cd "$RF/trace-decoder" && cargo test --quiet >/dev/null 2>&1); then
    echo "  ok: trace-decoder 8"
else
    echo "  FAIL: trace-decoder"; FAIL=1
fi

if [ $FAIL -eq 0 ]; then
    echo "ALL TESTS PASSED (mkpms 19 suites + kernel 14 + rustFrida 18)"
else
    echo "SOME TESTS FAILED"; exit 1
fi
