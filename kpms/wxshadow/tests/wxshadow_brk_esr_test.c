/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow BRK/ESR 解码逻辑执行级测试。
 *
 * 验证断点机制核心：
 *   1. BRK 指令编码（WXSHADOW_BRK_INSN 是否匹配 A64 手册）
 *   2. ESR 异常分类（exec/read/write permission fault 区分）
 *   3. BRK imm 匹配（handler 如何识别自己的断点）
 * 副本需与 wxshadow.h / wxshadow_internal.h 同步。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_esr_test kpms/wxshadow/tests/wxshadow_brk_esr_test.c
 *   /tmp/wx_esr_test
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned long long u64;

/* ---- 同步自 wxshadow.h ---- */
#define WXSHADOW_BRK_IMM        0x007
#define AARCH64_BREAK_MON       0xd4200000
#define WXSHADOW_BRK_INSN       (AARCH64_BREAK_MON | (WXSHADOW_BRK_IMM << 5))
#define AARCH64_INSN_SIZE       4

/* ---- 同步自 wxshadow_internal.h ---- */
#define ESR_ELx_EC_SHIFT        26
#define ESR_ELx_EC_MASK         (0x3FUL << ESR_ELx_EC_SHIFT)
#define ESR_ELx_EC(esr)         (((esr) & ESR_ELx_EC_MASK) >> ESR_ELx_EC_SHIFT)
#define ESR_ELx_IL_SHIFT        25
#define ESR_ELx_IL              (1UL << ESR_ELx_IL_SHIFT)
#define ESR_ELx_ISS_MASK        0x01FFFFFFUL
#define ESR_ELx_WNR_SHIFT       6
#define ESR_ELx_WNR             (1UL << ESR_ELx_WNR_SHIFT)
#define ESR_ELx_S1PTW_SHIFT     7
#define ESR_ELx_S1PTW           (1UL << ESR_ELx_S1PTW_SHIFT)
#define ESR_ELx_CM_SHIFT        8
#define ESR_ELx_CM              (1UL << ESR_ELx_CM_SHIFT)
#define ESR_ELx_EC_UNKNOWN      0x00
#define ESR_ELx_EC_IABT_LOW     0x20
#define ESR_ELx_EC_IABT_CUR     0x21
#define ESR_ELx_EC_DABT_LOW     0x24
#define ESR_ELx_EC_DABT_CUR     0x25

static int is_el0_instruction_abort(unsigned int esr)
{
    return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW;
}
static int is_el0_data_abort(unsigned int esr)
{
    return ESR_ELx_EC(esr) == ESR_ELx_EC_DABT_LOW;
}
static int is_permission_fault(unsigned int esr)
{
    unsigned int fsc = esr & 0x3F;
    return (fsc & 0x3C) == 0x0C;
}
enum { FAULT_NONE = 0, FAULT_EXEC, FAULT_READ, FAULT_WRITE };
static int classify_permission_fault(unsigned int esr)
{
    if (!is_permission_fault(esr))
        return FAULT_NONE;
    if (is_el0_instruction_abort(esr))
        return FAULT_EXEC;
    if (!is_el0_data_abort(esr))
        return FAULT_NONE;
    if (esr & ESR_ELx_S1PTW)
        return FAULT_NONE;
    if (esr & ESR_ELx_CM)
        return FAULT_READ;
    return (esr & ESR_ELx_WNR) ? FAULT_WRITE : FAULT_READ;
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    /* 1. BRK 编码：imm 位 [20:5]，与手册一致 */
    CHECK(WXSHADOW_BRK_INSN == 0xd42000e0UL, "BRK #7 == 0xd42000e0 (A64 manual)");
    CHECK(((WXSHADOW_BRK_INSN >> 5) & 0xFFFF) == 0x7, "BRK imm field = 7");

    /* 2. BRK 指令大小（断点步进用） */
    CHECK(AARCH64_INSN_SIZE == 4, "insn size 4");

    /* 3. ESR 分类：构造典型 ESR */
    {
        unsigned int iabt_perm = (ESR_ELx_EC_IABT_LOW << ESR_ELx_EC_SHIFT) | 0x0C; /* 执行权限 */
        unsigned int dabt_write = (ESR_ELx_EC_DABT_LOW << ESR_ELx_EC_SHIFT) | (1 << 6) | 0x0C; /* 写 */
        unsigned int dabt_read  = (ESR_ELx_EC_DABT_LOW << ESR_ELx_EC_SHIFT) | 0x0C; /* 读 */
        unsigned int normal_fault = (ESR_ELx_EC_DABT_LOW << ESR_ELx_EC_SHIFT) | 0x04; /* 非权限 */

        CHECK(is_el0_instruction_abort(iabt_perm), "IABT_LOW detected");
        CHECK(is_el0_data_abort(dabt_write), "DABT_LOW detected");
        CHECK(is_permission_fault(iabt_perm), "IABT perm fault");
        CHECK(is_permission_fault(dabt_write), "DABT perm fault");
        CHECK(!is_permission_fault(normal_fault), "normal fault not perm");

        CHECK(classify_permission_fault(iabt_perm) == FAULT_EXEC, "IABT -> EXEC");
        CHECK(classify_permission_fault(dabt_write) == FAULT_WRITE, "DABT+WNR -> WRITE");
        CHECK(classify_permission_fault(dabt_read) == FAULT_READ, "DABT no WNR -> READ");
        CHECK(classify_permission_fault(normal_fault) == FAULT_NONE, "normal -> NONE");
    }

    /* 4. ESR 的 IL 位与 ISS 提取 */
    {
        unsigned int esr = (1UL << ESR_ELx_IL_SHIFT) | 0x12345;
        CHECK(esr & ESR_ELx_IL, "IL bit present");
        CHECK((esr & ESR_ELx_ISS_MASK) == 0x12345, "ISS extraction");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
