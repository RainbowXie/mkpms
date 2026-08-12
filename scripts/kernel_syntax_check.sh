#!/bin/bash
# kernel_syntax_check.sh — 用真实 KernelPatch 框架头对 KPM 内核源文件做语法级核查。
#
# 背景：本环境缺 aarch64-linux-gnu-gcc 且 .kp submodule 未初始化，无法完整编译
# 内核模块。本脚本用 host gcc -fsyntax-only + KernelPatch 源码头（本地或 submodule
# 检出）做预处理级验证，能发现类型/签名/宏错误。补充说明见 ASSUMPTIONS.md。
#
# 用法:
#   ./kernel_syntax_check.sh [KP_ROOT]   # KP_ROOT 默认 ./kernel（submodule 路径）
#
# 退出码: 0 = 全部通过；非 0 = 有错误。

set -u
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KP_ROOT="${1:-$REPO_DIR/kernel}"

# KernelPatch 框架头目录（源码树或 submodule 检出）
# 兼容两种布局：KP_ROOT/include（submodule 顶层）或 KP_ROOT/kernel/include（源码树）
KP_INC_ROOT="$KP_ROOT"
if [ -f "$KP_ROOT/kernel/include/kpmodule.h" ]; then
    KP_INC_ROOT="$KP_ROOT/kernel"
elif [ ! -f "$KP_ROOT/include/kpmodule.h" ]; then
    echo "ERROR: KernelPatch headers not found at $KP_ROOT"
    echo "  → 初始化 submodule: git submodule update --init .kp"
    echo "  → 或指定本地源码: $0 /mnt/data/Work/Projects/KernelPatch"
    exit 2
fi

INCS=(
    "$KP_INC_ROOT/include"
    "$KP_INC_ROOT/patch/include"
    "$KP_INC_ROOT/linux/include"
    "$KP_INC_ROOT/linux/arch/arm64/include"
    "$KP_INC_ROOT/linux/tools/arch/arm64/include"
)

INC_ARGS=""
for d in "${INCS[@]}"; do
    [ -d "$d" ] && INC_ARGS="$INC_ARGS -I$d"
done

# 目标文件：全部 KPM 模块内核源（排除用户态 *_client.c 与 common/ 共享头目录）
FILES=""
for m in kpms/*/; do
    [ -d "$m" ] || continue
    case "$m" in
        *common/) continue ;;
    esac
    for f in "$REPO_DIR/$m"*.c; do
        [ -f "$f" ] || continue
        case "$f" in
            *_client.c) continue ;;
        esac
        FILES="$FILES $f"
    done
done

FAIL=0
for f in $FILES; do
    if gcc -fsyntax-only -D__aarch64__ -nostdinc $INC_ARGS "$f" 2>/tmp/ksc_err.txt; then
        echo "ok: $f"
    else
        echo "FAIL: $f"
        head -8 /tmp/ksc_err.txt
        FAIL=1
    fi
done

if [ $FAIL -eq 0 ]; then
    echo "ALL KERNEL SYNTAX OK"
fi
exit $FAIL
