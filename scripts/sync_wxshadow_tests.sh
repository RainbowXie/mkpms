#!/bin/bash
# sync_wxshadow_tests.sh — 从 wxshadow 源同步 PTE/BRK-ESR 测试副本。
# wxshadow 的 static 函数无法直接 include（依赖内核头），测试用同步副本；
# 修改 wxshadow_pgtable.c / wxshadow_internal.h / wxshadow.h 后运行本脚本，
# 手动把改动同步进测试文件（目前为半自动：运行后核对差异）。
#
# 用法: ./scripts/sync_wxshadow_tests.sh
set -e
cd "$(dirname "$0")/.."

echo "wxshadow 测试副本需手动同步（static 函数无法 include）："
echo "  - kpms/wxshadow/tests/wxshadow_pte_state_test.c  ← wxshadow_pgtable.c"
echo "    (make_pte / replace_pte_pfn / restore / hidden / stepping)"
echo "  - kpms/wxshadow/tests/wxshadow_brk_esr_test.c  ← wxshadow.h + wxshadow_internal.h"
echo "    (WXSHADOW_BRK_INSN / ESR_ELx_* 宏与分类函数)"
echo
echo "同步核对提示："
grep -n "WXSHADOW_BRK_INSN" kpms/wxshadow/wxshadow.h | head -1
grep -n "WXSHADOW_BRK_INSN" kpms/wxshadow/tests/wxshadow_brk_esr_test.c | head -1
echo
echo "若两处值一致则副本未漂移。"
