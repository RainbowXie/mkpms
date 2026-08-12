/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow TLB flush 三级回退优先级执行级测试。
 *
 * 验证不同内核符号可用性下选择正确的 flush 路径：
 *   1. kfunc_flush_tlb_page 可用 → 用它
 *   2. 否则 __flush_tlb_range 可用 → 用它（last_level=true, level=3）
 *   3. 否则 TLBI 指令直发（需 vma->mm 或 broadcast）
 * 逻辑抽取自 wxshadow_pgtable.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_tlb_test kpms/wxshadow/tests/wxshadow_tlb_test.c
 *   /tmp/wx_tlb_test
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned long long u64;
typedef unsigned long size_t;
#define PAGE_SIZE 4096UL
#define __user

/* ---- kfunc 桩（可开关 + 计数器） ---- */
static int calls_flush_tlb_page = 0;
static int calls_flush_range = 0;
static int calls_tlbi = 0;
static void *stub_flush_tlb_page = (void *)1; /* 非 NULL = 可用 */
static void *stub_flush_tlb_range = (void *)1;
static int stub_avail_page = 1, stub_avail_range = 1;

static void fake_flush_tlb_page(void *vma, unsigned long uaddr)
{
    (void)vma; (void)uaddr;
    calls_flush_tlb_page++;
}
static void fake_flush_tlb_range(void *vma, unsigned long start, unsigned long end,
                                 unsigned long stride, int last_level, int tlb_level)
{
    (void)vma; (void)start; (void)end; (void)stride; (void)last_level; (void)tlb_level;
    calls_flush_range++;
}

/* 按可用性返回 kfunc 指针（模拟符号解析结果） */
#define kfunc_flush_tlb_page (stub_avail_page ? fake_flush_tlb_page : 0)
#define kfunc___flush_tlb_range (stub_avail_range ? fake_flush_tlb_range : 0)

/* vma 桩：vm_mm 偏移 0x40 处为 mm 指针 */
struct stub_vma { unsigned char pad[0x40]; void *mm; };
#define VMA_VM_MM_OFFSET 0x40
static void *vma_mm(void *vma)
{
    return *(void **)((char *)vma + VMA_VM_MM_OFFSET);
}

/* TLBI 指令在 x86 host 不可汇编：用计数器桩替代调用目标 */
static void wxshadow_tlbi_page(void *mm, unsigned long uaddr)
{
    (void)mm; (void)uaddr;
    calls_tlbi++;
}

/* ---- 同步自 wxshadow_pgtable.c ---- */
void wxshadow_flush_tlb_page(void *vma, unsigned long uaddr)
{
    if (kfunc_flush_tlb_page) {
        kfunc_flush_tlb_page(vma, uaddr);
    } else if (kfunc___flush_tlb_range) {
        kfunc___flush_tlb_range(vma, uaddr, uaddr + PAGE_SIZE, PAGE_SIZE, 1 /*true*/, 3);
    } else {
        void *mm = vma ? vma_mm(vma) : NULL;
        wxshadow_tlbi_page(mm, uaddr);
    }
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    struct stub_vma vma = { .mm = (void *)0xdeadbeef };

    /* 1. 两者都可用 → flush_tlb_page 优先 */
    calls_flush_tlb_page = calls_flush_range = calls_tlbi = 0;
    wxshadow_flush_tlb_page(&vma, 0x1000);
    CHECK(calls_flush_tlb_page == 1, "flush_tlb_page used when available");
    CHECK(calls_flush_range == 0 && calls_tlbi == 0, "range/tlbi not used");

    /* 2. flush_tlb_page 不可用 → __flush_tlb_range */
    stub_avail_page = 0;
    calls_flush_tlb_page = calls_flush_range = calls_tlbi = 0;
    wxshadow_flush_tlb_page(&vma, 0x2000);
    CHECK(calls_flush_range == 1, "__flush_tlb_range used as fallback");
    CHECK(calls_flush_tlb_page == 0 && calls_tlbi == 0, "page/tlbi not used");

    /* 3. 两者都不可用 → TLBI 指令（vma 有 mm） */
    stub_avail_range = 0;
    calls_flush_tlb_page = calls_flush_range = calls_tlbi = 0;
    wxshadow_flush_tlb_page(&vma, 0x3000);
    CHECK(calls_tlbi == 1, "TLBI used as final fallback");

    /* 4. 全不可用 + vma=NULL → TLBI 仍发（mm=NULL 由 tlbi_page 处理） */
    calls_flush_tlb_page = calls_flush_range = calls_tlbi = 0;
    wxshadow_flush_tlb_page(NULL, 0x4000);
    CHECK(calls_tlbi == 1, "TLBI used with NULL vma");

    /* 5. 恢复可用性 */
    stub_avail_page = 1; stub_avail_range = 1;

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
