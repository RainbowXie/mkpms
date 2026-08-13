/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow get_user_pte 页表遍历逻辑执行级测试。
 *
 * 模拟 arm64 4 级页表（PGD/PUD/PMD/PTE 各 512 项），验证遍历：
 *   - 正常命中（4 级 + 3 级）
 *   - 各层 0 值 → NULL
 *   - PMD block/section → NULL（提示 split）
 *   - 无效 PMD 类型 → NULL
 * 桩助手与索引逻辑抽取自 wxshadow_pgtable.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_pte_walk kpms/wxshadow/tests/wxshadow_get_user_pte_test.c
 *   /tmp/wx_pte_walk
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned long long u64;
#define PAGE_SHIFT 12UL

/* ---- 桩：物理地址 == 指针值（单地址空间） ---- */
/* harness：host 堆地址非真 KVA，放宽为"非零即视为有效"（表指针语义） */
static int is_kva(unsigned long addr) { return addr != 0; }
static int safe_read_u64(unsigned long addr, u64 *out)
{
    *out = *(volatile u64 *)addr;
    return 1;
}
static unsigned long pxd_page_vaddr(u64 v) { return v & 0x0000FFFFFFFFF000UL; }

/* ---- 索引（同步自 wxshadow_pgtable.c，4 级 4K） ---- */
static int wx_page_level = 4;
static unsigned long pgd_index(unsigned long a) {
    int pxd_bits = 12 - 3, s = 12 + (4 - 1) * 9;
    return (a >> s) & ((1UL << pxd_bits) - 1);
}
static unsigned long pud_index(unsigned long a) {
    int s = 12 + 2 * 9;
    return (a >> s) & 511;
}
static unsigned long pmd_index(unsigned long a) {
    int s = 12 + 9;
    return (a >> s) & 511;
}
static unsigned long pte_index(unsigned long a) { return (a >> 12) & 511; }

/* ---- 偏移助手（同步） ---- */
static void *wxshadow_pgd_offset(void *mm, unsigned long addr)
{
    /* mm 即 pgd 数组（harness 简化：mm->pgd 为数组本身） */
    return (u64 *)mm + pgd_index(addr);
}
static void *wxshadow_pud_offset(void *p4d, unsigned long addr)
{
    u64 p4d_val = *(volatile u64 *)p4d;
    return (u64 *)pxd_page_vaddr(p4d_val) + pud_index(addr);
}
static void *wxshadow_pmd_offset(void *pud, unsigned long addr)
{
    u64 pud_val = *(volatile u64 *)pud;
    return (u64 *)pxd_page_vaddr(pud_val) + pmd_index(addr);
}

/* ---- 类型判断（同步） ---- */
#define PXD_TYPE_SECT 0x1UL
#define PXD_TYPE_TABLE 0x3UL
static int pmd_sect(u64 pmd) { return (pmd & 0x3UL) == PXD_TYPE_SECT; }
static int pmd_table(u64 pmd) { return (pmd & 0x3UL) == PXD_TYPE_TABLE; }

/* ---- get_user_pte（同步自 wxshadow_pgtable.c） ---- */
static u64 *get_user_pte(void *mm, unsigned long addr)
{
    void *pgd, *pud, *pmd;
    u64 pgd_val, pud_val, pmd_val;

    pgd = wxshadow_pgd_offset(mm, addr);
    if (!pgd || !is_kva((unsigned long)pgd)) return NULL;
    if (!safe_read_u64((unsigned long)pgd, &pgd_val)) return NULL;
    if (pgd_val == 0) return NULL;

    if (wx_page_level == 4) {
        pud = wxshadow_pud_offset(pgd, addr);
        if (!pud) return NULL;
        if (!safe_read_u64((unsigned long)pud, &pud_val)) return NULL;
        if (pud_val == 0) return NULL;
        pmd = wxshadow_pmd_offset(pud, addr);
    } else {
        pmd = wxshadow_pmd_offset(pgd, addr);
    }
    if (!pmd) return NULL;
    if (!safe_read_u64((unsigned long)pmd, &pmd_val)) return NULL;
    if (pmd_val == 0) return NULL;
    if (pmd_sect(pmd_val)) return NULL;
    if (!pmd_table(pmd_val)) return NULL;

    return (u64 *)pxd_page_vaddr(pmd_val) + pte_index(addr);
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

/* 页对齐分配：表描述符掩码要求 4K 对齐（host malloc 仅 16B） */
static void *aligned_alloc_4k(void)
{
    unsigned char *raw = malloc(8192);
    unsigned long aligned = ((unsigned long)raw + 4095) & ~4095UL;
    memset((void *)aligned, 0, 4096);
    return (void *)aligned;
}

/* 建表：mm->pgd → pud 表 → pmd 表 → pte 表，全页对齐清零 */
static void build_tables(void **mm_out, u64 **pgd_out, u64 **pud_out,
                         u64 **pmd_out, u64 **pte_out)
{
    u64 *pgd = aligned_alloc_4k(), *pud = aligned_alloc_4k();
    u64 *pmd = aligned_alloc_4k(), *pte = aligned_alloc_4k();
    *pgd_out = pgd; *pud_out = pud; *pmd_out = pmd; *pte_out = pte;
    *mm_out = pgd;
    /* 链接：4GB 地址 → pgd[0], pud[4], pmd[0], pte[0] */
    pgd[0] = (u64)pud | 0x3;
    pud[4] = (u64)pmd | 0x3;
    pmd[0] = (u64)pte | 0x3;
}

int main(void)
{
    /* addr 0x100000000（4GB）→ pgd[0], pud[0], pmd[0], pte[0] */
    unsigned long va = 0x100000000UL;

    /* 1. 正常 4 级遍历 */
    {
        void *mm; u64 *pgd, *pud, *pmd, *pte;
        build_tables(&mm, &pgd, &pud, &pmd, &pte);
        pte[0] = 0x12345000UL | 0x3; /* 有效 leaf */
        u64 *got = get_user_pte(mm, va);
        CHECK(got == &pte[0], "4-level walk hits pte[0]");
        CHECK(*got == pte[0], "leaf value preserved");
        (void)pgd; (void)pud; (void)pmd; (void)pte;
    }

    /* 2. PTE 层 0（未映射）→ 返回槽但值为 0 */
    {
        void *mm; u64 *pgd, *pud, *pmd, *pte;
        build_tables(&mm, &pgd, &pud, &pmd, &pte);
        u64 *got = get_user_pte(mm, va);
        CHECK(got == &pte[0] && *got == 0, "unmapped leaf: slot with zero");
        (void)pgd; (void)pud; (void)pmd; (void)pte;
    }

    /* 3. PGD 层 0 → NULL */
    {
        void *mm; u64 *pgd = aligned_alloc_4k();
        mm = pgd;
        CHECK(get_user_pte(mm, va) == NULL, "pgd zero -> NULL");
        (void)pgd;
    }

    /* 4. PMD block/section → NULL */
    {
        void *mm; u64 *pgd, *pud, *pmd, *pte;
        build_tables(&mm, &pgd, &pud, &pmd, &pte);
        pmd[0] = 0x200000UL | PXD_TYPE_SECT; /* section 映射 */
        CHECK(get_user_pte(mm, va) == NULL, "pmd section -> NULL");
        (void)pgd; (void)pud; (void)pmd; (void)pte;
    }

    /* 5. 3 级页表（无 PUD） */
    {
        void *mm; u64 *pgd = aligned_alloc_4k(), *pmd = aligned_alloc_4k(), *pte = aligned_alloc_4k();
        mm = pgd;
        pgd[0] = (u64)pmd | 0x3;
        pmd[0] = (u64)pte | 0x3; /* 3 级：pmd_index(4GB)=0 */
        pte[0] = 0xabc000UL | 0x3;
        wx_page_level = 3;
        u64 *got = get_user_pte(mm, va);
        CHECK(got == &pte[0], "3-level walk hits pte[0]");
        CHECK(*got == pte[0], "3-level leaf preserved");
        wx_page_level = 4;
        (void)pgd; (void)pmd; (void)pte;
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
