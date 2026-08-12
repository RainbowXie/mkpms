/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow 地址翻译助手执行级测试。
 *
 * 覆盖：
 *   - is_kva（ARM64 内核 VA 高位 0xffff 判断）
 *   - phys_to_virt_safe / kaddr_to_phys 三种模式：
 *       a) physvirt_offset_valid（AT 指令检测模式）
 *       b) kvar_physvirt_offset（KASLR 模式）
 *       c) memstart_addr + page_offset_base（传统模式）
 *     且 v↔p 互逆（round-trip）。
 * 逻辑抽取自 wxshadow_internal.h（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_addr_test kpms/wxshadow/tests/wxshadow_addr_test.c
 *   /tmp/wx_addr_test
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned long long u64;
typedef long long s64;

/* ---- 模式开关 ---- */
static int physvirt_offset_valid = 0;
static s64 detected_physvirt_offset = 0;
static s64 *kvar_physvirt_offset = 0;
static s64 *kvar_memstart_addr = 0;
static unsigned long page_offset_base = 0;

/* ---- 同步自 wxshadow_internal.h ---- */
static int is_kva(unsigned long addr)
{
    return (addr >> 48) == 0xffff;
}

static unsigned long phys_to_virt_safe(unsigned long pa)
{
    if (physvirt_offset_valid)
        return pa + detected_physvirt_offset;
    else if (kvar_physvirt_offset)
        return pa + *kvar_physvirt_offset;
    else
        return (pa - *kvar_memstart_addr) + page_offset_base;
}

static unsigned long kaddr_to_phys(unsigned long vaddr)
{
    if (physvirt_offset_valid)
        return vaddr - detected_physvirt_offset;
    else if (kvar_physvirt_offset)
        return vaddr - *kvar_physvirt_offset;
    else
        return (vaddr - page_offset_base) + *kvar_memstart_addr;
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    /* 1. is_kva 边界 */
    CHECK(is_kva(0xffff000000000000UL), "kernel VA high");
    CHECK(is_kva(0xffff8000a0000000UL), "kernel VA typical");
    CHECK(!is_kva(0x0000ffff00000000UL), "user VA not kva");
    CHECK(!is_kva(0x7f0000000000UL), "user low not kva");

    /* 2a. physvirt 检测模式 */
    physvirt_offset_valid = 1;
    detected_physvirt_offset = 0xffff000000000000LL; /* 典型线性映射偏移 */
    {
        unsigned long va = phys_to_virt_safe(0x10000000UL);
        CHECK(va == 0xffff000010000000UL, "physvirt: pa->va");
        CHECK(kaddr_to_phys(va) == 0x10000000UL, "physvirt: va->pa roundtrip");
    }

    /* 2b. kvar_physvirt_offset（KASLR）模式 */
    physvirt_offset_valid = 0;
    {
        static s64 kv = 0xffff3f8000000000LL; /* 随机化偏移 */
        kvar_physvirt_offset = &kv;
        unsigned long va = phys_to_virt_safe(0x2000UL);
        CHECK(va == 0xffff3f8000002000UL, "kvar: pa->va");
        CHECK(kaddr_to_phys(va) == 0x2000UL, "kvar: va->pa roundtrip");
        kvar_physvirt_offset = 0;
    }

    /* 2c. memstart 传统模式 */
    {
        static s64 ms = 0x40000000LL; /* memstart_addr 示例 */
        page_offset_base = 0xffff000000000000UL;
        kvar_memstart_addr = &ms;
        unsigned long va = phys_to_virt_safe(0x40000000UL);
        CHECK(va == 0xffff000000000000UL, "memstart: pa->va (pa==memstart)");
        unsigned long va2 = phys_to_virt_safe(0x50000000UL);
        CHECK(va2 == 0xffff000010000000UL, "memstart: pa->va offset");
        CHECK(kaddr_to_phys(va2) == 0x50000000UL, "memstart: va->pa roundtrip");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
