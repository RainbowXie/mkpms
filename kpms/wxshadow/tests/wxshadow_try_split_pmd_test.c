/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow try_split_pmd（THP 大页分裂判定）执行级测试。
 *
 * 验证：PMD block/section 检测 + __split_huge_pmd 调用 + 分裂验证。
 *   - 非 section（普通表）→ 无事可做返回 0
 *   - section + 有 __split_huge_pmd → 分裂并验证
 *   - section + 无 __split_huge_pmd → -ENOSYS
 * 逻辑抽取自 wxshadow_pgtable.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_split kpms/wxshadow/tests/wxshadow_try_split_pmd_test.c
 *   /tmp/wx_split
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned long long u64;
#define PAGE_SHIFT 12UL

/* ---- 桩 ---- */
static int is_kva(unsigned long addr) { return addr != 0; }
static int safe_read_u64(unsigned long addr, u64 *out)
{
    *out = *(volatile u64 *)addr;
    return 1;
}
static unsigned long pxd_page_vaddr(u64 v) { return v & 0x0000FFFFFFFFF000UL; }
static int wx_page_level = 4;
static int wx_page_shift = 12;
static unsigned long pgd_index(unsigned long a) { int s = 12 + 3 * 9; return (a >> s) & 511; }
static unsigned long pud_index(unsigned long a) { int s = 12 + 2 * 9; return (a >> s) & 511; }
static unsigned long pmd_index(unsigned long a) { int s = 12 + 9; return (a >> s) & 511; }
static void *wxshadow_pgd_offset(void *mm, unsigned long addr) { return (u64 *)mm + pgd_index(addr); }
static void *wxshadow_pud_offset(void *p4d, unsigned long addr)
{ u64 v = *(volatile u64 *)p4d; return (u64 *)pxd_page_vaddr(v) + pud_index(addr); }
static void *wxshadow_pmd_offset(void *pud, unsigned long addr)
{ u64 v = *(volatile u64 *)pud; return (u64 *)pxd_page_vaddr(v) + pmd_index(addr); }
#define PXD_TYPE_SECT 0x1UL
static int pmd_sect(u64 pmd) { return (pmd & 0x3UL) == PXD_TYPE_SECT; }
static void *aligned_alloc_4k(void)
{ unsigned char *raw = malloc(8192); unsigned long a = ((unsigned long)raw + 4095) & ~4095UL;
  memset((void *)a, 0, 4096); return (void *)a; }

/* __split_huge_pmd 桩：可计数/可开关 */
static int split_calls = 0;
static int split_available = 1;
static int split_should_fail = 0;
static void split_huge_pmd_stub(void *vma, u64 *pmd, unsigned long addr,
                                int freeze, void *page)
{
    (void)vma; (void)addr; (void)freeze; (void)page;
    split_calls++;
    if (split_should_fail) {
        /* 分裂失败：保持 section */
    } else {
        *pmd = 0x0; /* 分裂成功：清 PMD（模拟） */
    }
}

/* try_split_pmd（同步自 wxshadow_pgtable.c） */
static int try_split_pmd(void *mm, void *vma, unsigned long addr)
{
    void *pgd, *pud, *pmd;
    u64 pgd_val, pud_val, pmd_val;

    if (!mm || !vma) return 0;
    pgd = wxshadow_pgd_offset(mm, addr);
    if (!pgd || !is_kva((unsigned long)pgd)) return 0;
    if (!safe_read_u64((unsigned long)pgd, &pgd_val) || pgd_val == 0) return 0;
    if (wx_page_level == 4) {
        pud = wxshadow_pud_offset(pgd, addr);
        if (!pud) return 0;
        if (!safe_read_u64((unsigned long)pud, &pud_val) || pud_val == 0) return 0;
        pmd = wxshadow_pmd_offset(pud, addr);
    } else {
        pmd = wxshadow_pmd_offset(pgd, addr);
    }
    if (!pmd) return 0;
    if (!safe_read_u64((unsigned long)pmd, &pmd_val) || pmd_val == 0) return 0;
    if (!pmd_sect(pmd_val)) return 0; /* 非 block，无事可做 */
    if (!split_available) return -38; /* ENOSYS */

    {
        int pxd_bits = wx_page_shift - 3;
        unsigned long pmd_shift_val = wx_page_shift + 1 * pxd_bits;
        unsigned long block_mask = ~((1UL << pmd_shift_val) - 1);
        split_huge_pmd_stub(vma, (u64 *)pmd, addr & block_mask, 0, NULL);
    }
    /* 验证 */
    if (!safe_read_u64((unsigned long)pmd, &pmd_val)) return -14;
    if (pmd_sect(pmd_val)) return -14; /* 仍 block → 失败 */
    return 0;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    unsigned long va = 0x100000000UL; /* 4GB: pgd[0], pud[4], pmd[0] */
    void *vma = (void *)0x1;

    /* 1. 非 section（普通表）→ 无事可做返回 0 */
    {
        u64 *pgd = aligned_alloc_4k(), *pud = aligned_alloc_4k(), *pmd = aligned_alloc_4k();
        pgd[0] = (u64)pud | 0x3; pud[4] = (u64)pmd | 0x3; pmd[0] = 0x12345000UL | 0x3; /* 表 */
        split_calls = 0;
        CHECK(try_split_pmd(pgd, vma, va) == 0, "non-section: no split needed");
        CHECK(split_calls == 0, "split not called");
    }

    /* 2. section + split 可用 → 分裂并验证 */
    {
        u64 *pgd = aligned_alloc_4k(), *pud = aligned_alloc_4k(), *pmd = aligned_alloc_4k();
        pgd[0] = (u64)pud | 0x3; pud[4] = (u64)pmd | 0x3;
        pmd[0] = 0x200000UL | PXD_TYPE_SECT; /* section 映射 */
        split_calls = 0; split_available = 1; split_should_fail = 0;
        CHECK(try_split_pmd(pgd, vma, va) == 0, "section split succeeds");
        CHECK(split_calls == 1, "split called once");
    }

    /* 3. section + 无 split 函数 → -ENOSYS */
    {
        u64 *pgd = aligned_alloc_4k(), *pud = aligned_alloc_4k(), *pmd = aligned_alloc_4k();
        pgd[0] = (u64)pud | 0x3; pud[4] = (u64)pmd | 0x3;
        pmd[0] = 0x200000UL | PXD_TYPE_SECT;
        split_available = 0;
        CHECK(try_split_pmd(pgd, vma, va) == -38, "no split fn -> ENOSYS");
        split_available = 1;
    }

    /* 4. section + 分裂失败（仍 block）→ -14 */
    {
        u64 *pgd = aligned_alloc_4k(), *pud = aligned_alloc_4k(), *pmd = aligned_alloc_4k();
        pgd[0] = (u64)pud | 0x3; pud[4] = (u64)pmd | 0x3;
        pmd[0] = 0x200000UL | PXD_TYPE_SECT;
        split_should_fail = 1;
        CHECK(try_split_pmd(pgd, vma, va) == -14, "split still-block -> -14");
        split_should_fail = 0;
    }

    /* 5. 页表层缺失 → 0（无事可做） */
    {
        u64 *pgd = aligned_alloc_4k();
        CHECK(try_split_pmd(pgd, vma, va) == 0, "pgd zero -> no-op");
    }

    /* 6. mm/vma NULL → 0 */
    CHECK(try_split_pmd(NULL, vma, va) == 0, "null mm -> no-op");
    CHECK(try_split_pmd((void *)0x1000, NULL, va) == 0, "null vma -> no-op");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
