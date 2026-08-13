/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow 跨仓库常量对齐自检。
 * 核对 mkpms 内核头与 rustFrida quickjs-hook 侧的 prctl 常量一致，
 * 防止跨仓库漂移（hook(ptr, cb, stealth) → wxshadow_patch 依赖此对齐）。
 *
 * 编译运行：
 *   gcc -Ikpms/wxshadow -o /tmp/wx_abi_test kpms/wxshadow/tests/wxshadow_abi_test.c
 *   /tmp/wx_abi_test
 */
#include <stdio.h>

/* 桩类型（wxshadow.h 引用内核结构，仅常量测试需要最小定义） */
struct list_head { struct list_head *next, *prev; };
typedef struct { int counter; } atomic_t;

/* mkpms 内核头 */
#include "wxshadow.h"

/* rustFrida quickjs-hook 侧（hook_engine_internal.h）的常量副本 */
#define RF_PATCH   0x57580006
#define RF_RELEASE 0x57580008

/* rustFrida 语义：PATCH 调用 prctl(PATCH, pid, addr, buf, len) */
#define RF_PATCH_ARGC 5

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    CHECK(PR_WXSHADOW_PATCH == RF_PATCH, "PATCH == 0x57580006 (mkpms==rustFrida)");
    CHECK(PR_WXSHADOW_RELEASE == RF_RELEASE, "RELEASE == 0x57580008 (mkpms==rustFrida)");
    CHECK(PR_WXSHADOW_SET_BP == 0x57580001, "SET_BP == 0x57580001");
    CHECK(PR_WXSHADOW_SET_REG == 0x57580002, "SET_REG == 0x57580002");
    CHECK(PR_WXSHADOW_DEL_BP == 0x57580003, "DEL_BP == 0x57580003");
    CHECK(PR_WXSHADOW_SET_TLB_MODE == 0x57580004, "SET_TLB_MODE == 0x57580004");
    CHECK(PR_WXSHADOW_GET_TLB_MODE == 0x57580005, "GET_TLB_MODE == 0x57580005");
    /* 空档：0x57580007 历史为 PR_WXSHADOW_ACTIVE（1.1.0），已移除 */
    CHECK(PR_WXSHADOW_RELEASE - PR_WXSHADOW_PATCH == 2, "PATCH..RELEASE gap (0x07 retired)");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
