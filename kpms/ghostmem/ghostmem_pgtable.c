/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem KPM Module - Page Table Operations
 *
 * 手动构建用户态 PTE 页表项：从 mm->pgd 逐级下探（PGD -> PUD -> PMD -> PTE），
 * 中间表缺失时分配新的表页（VMA-Less 空洞里没有现成页表）。解除映射时逐级
 * 回收已空的表页，避免内核内存泄漏。
 *
 * Copyright (C) 2024
 */

#include "ghostmem_internal.h"

/* PTE table page covers 512 PTEs (2MB for 4K pages) */
#define GH_PTE_COVER_SIZE (512UL << GHOSTMEM_PAGE_SHIFT)
#define GH_PTE_COVER_MASK (~(GH_PTE_COVER_SIZE - 1))

/* ========== Walk helpers ========== */

/*
 * Clean dcache to PoU for a kernel VA range (wxshadow 同款实现)。
 * 表页由 ARM64 表 walker 读取：写入表描述符后必须 dc cvau，否则
 * walker 可能读到脏 dcache 行（尤其跨核 / 无缓存一致性场景）。
 */
static void ghostmem_flush_kern_dcache(unsigned long kva, unsigned long size)
{
    unsigned long addr, end;
    u64 ctr_el0, line_size;

    asm volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    line_size = 4 << ((ctr_el0 >> 16) & 0xf);

    end = kva + size;
    for (addr = kva & ~(line_size - 1); addr < end; addr += line_size)
        asm volatile("dc cvau, %0" : : "r"(addr) : "memory");
    asm volatile("dsb ish" : : : "memory");
}

/*
 * Walk to the PTE entry for @addr without allocating intermediate tables.
 * Returns NULL when any level is missing or not a table descriptor.
 */
static u64 *ghostmem_get_pte(void *mm, unsigned long addr)
{
    u64 *pgd, *pud, *pmd;
    u64 pgd_val, pud_val, pmd_val;

    pgd = (u64 *)gh_mm_pgd(mm) + gh_pgd_index(addr);
    if (!gh_safe_read_u64((unsigned long)pgd, &pgd_val) || !pgd_val)
        return NULL;
    if ((pgd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return NULL;

    if (gh_page_level == 4) {
        pud = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pud_index(addr);
        if (!gh_safe_read_u64((unsigned long)pud, &pud_val) || !pud_val)
            return NULL;
        if ((pud_val & 0x3UL) != GH_PXD_TYPE_TABLE)
            return NULL;
        pmd = (u64 *)gh_pxd_page_vaddr(pud_val) + gh_pmd_index(addr);
    } else {
        pmd = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pmd_index(addr);
    }

    if (!gh_safe_read_u64((unsigned long)pmd, &pmd_val) || !pmd_val)
        return NULL;
    if ((pmd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return NULL;

    return (u64 *)gh_pxd_page_vaddr(pmd_val) + gh_pte_index(addr);
}

/*
 * Allocate a zeroed table page and link it into @parent (a PGD/PUD/PMD slot).
 * Returns the new table page kernel VA, or 0 on failure.
 */
static unsigned long ghostmem_alloc_table(u64 *parent)
{
    unsigned long kva = kfunc___get_free_pages(0xcc0 | 0x100 /* GFP_KERNEL|__GFP_ZERO */, 0);
    unsigned long phys;

    if (!kva)
        return 0;
    phys = gh_kaddr_to_phys(kva);
    *parent = (phys & 0x0000FFFFFFFFF000UL) | PTE_VALID | PTE_TABLE_BIT;
    /* 新表页写入的表描述符必须对表 walker 可见 */
    ghostmem_flush_kern_dcache(kva, GHOSTMEM_PAGE_SIZE);
    return kva;
}

/*
 * Walk to the PTE entry for @addr, allocating any missing intermediate table.
 * Returns the leaf PTE pointer, or NULL on allocation failure.
 */
static u64 *ghostmem_get_or_create_pte(void *mm, unsigned long addr)
{
    u64 *pgd, *pud, *pmd;
    u64 pgd_val, pud_val, pmd_val;

    pgd = (u64 *)gh_mm_pgd(mm) + gh_pgd_index(addr);
    if (!gh_safe_read_u64((unsigned long)pgd, &pgd_val) || !pgd_val) {
        if (!ghostmem_alloc_table(pgd))
            return NULL;
        pgd_val = *pgd;
    }
    if ((pgd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return NULL;

    if (gh_page_level == 4) {
        pud = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pud_index(addr);
        if (!gh_safe_read_u64((unsigned long)pud, &pud_val) || !pud_val) {
            if (!ghostmem_alloc_table(pud))
                return NULL;
            pud_val = *pud;
        }
        if ((pud_val & 0x3UL) != GH_PXD_TYPE_TABLE)
            return NULL;
        pmd = (u64 *)gh_pxd_page_vaddr(pud_val) + gh_pmd_index(addr);
    } else {
        pmd = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pmd_index(addr);
    }

    if (!gh_safe_read_u64((unsigned long)pmd, &pmd_val) || !pmd_val) {
        if (!ghostmem_alloc_table(pmd))
            return NULL;
        pmd_val = *pmd;
    }
    if ((pmd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return NULL;

    return (u64 *)gh_pxd_page_vaddr(pmd_val) + gh_pte_index(addr);
}

/* ========== TLB flush ========== */

/*
 * Broadcast TLB invalidation for one user VA (all ASIDs).
 * 不需要 VMA / ASID，幽灵内存没有对应 VMA，内核 flush_tlb_page 无法使用。
 */
static void ghostmem_tlbi_broadcast(unsigned long addr)
{
    asm volatile("tlbi vaale1is, %0" : : "r"(addr >> GHOSTMEM_PAGE_SHIFT) : "memory");
}

static void ghostmem_flush_range(unsigned long va, unsigned long nr_pages)
{
    unsigned long i;

    for (i = 0; i < nr_pages; i++)
        ghostmem_tlbi_broadcast(va + i * GHOSTMEM_PAGE_SIZE);
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

/* ========== Leaf PTE build ========== */

static u64 ghostmem_make_pte(unsigned long pfn, unsigned int prot)
{
    u64 pte = (pfn << GHOSTMEM_PAGE_SHIFT) | PTE_VALID | PTE_TYPE_PAGE |
              PTE_AF | PTE_SHARED | PTE_NG | PTE_ATTRINDX_NORMAL;

    /* 权限位：PTE_RDONLY=1 只读；PTE_UXN=1 不可执行 */
    if (!(prot & GHOSTMEM_PROT_WRITE))
        pte |= PTE_RDONLY;
    if (!(prot & GHOSTMEM_PROT_EXEC))
        pte |= PTE_UXN;
    return pte;
}

/* ========== Public API ========== */

int ghostmem_map_pages(void *mm, unsigned long va, unsigned long *pfns,
                       unsigned long nr_pages, unsigned int prot)
{
    unsigned long i;
    u64 *ptep;

    for (i = 0; i < nr_pages; i++) {
        ptep = ghostmem_get_or_create_pte(mm, va + i * GHOSTMEM_PAGE_SIZE);
        if (!ptep)
            return -ENOMEM;
        *ptep = ghostmem_make_pte(pfns[i], prot);
    }
    ghostmem_flush_range(va, nr_pages);
    return 0;
}

/*
 * Check whether a table page has any valid (present) entry.
 * 用于回收空表页：整页全空才可释放。
 */
static bool ghostmem_table_page_empty(const u64 *table)
{
    int i;

    for (i = 0; i < 512; i++) {
        if (table[i] & PTE_VALID)
            return false;
    }
    return true;
}

/*
 * Reclaim empty intermediate tables above one 2MB group, cascading upward.
 * 每次解除映射后调用；共享表页只有在全部条目都空时才被回收，因此多块共享安全。
 */
static void ghostmem_reclaim_tables(void *mm, unsigned long va)
{
    u64 *pgd, *pud, *pmd;
    u64 pgd_val, pud_val, pmd_val;
    u64 *pte_table;
    unsigned long pte_kva, pmd_kva = 0, pud_kva = 0;

    /* 找到该地址所在 PTE 表页 */
    pgd = (u64 *)gh_mm_pgd(mm) + gh_pgd_index(va);
    if (!gh_safe_read_u64((unsigned long)pgd, &pgd_val) || !pgd_val ||
        (pgd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return;

    if (gh_page_level == 4) {
        pud = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pud_index(va);
        if (!gh_safe_read_u64((unsigned long)pud, &pud_val) || !pud_val ||
            (pud_val & 0x3UL) != GH_PXD_TYPE_TABLE)
            return;
        pmd = (u64 *)gh_pxd_page_vaddr(pud_val) + gh_pmd_index(va);
    } else {
        pmd = (u64 *)gh_pxd_page_vaddr(pgd_val) + gh_pmd_index(va);
    }
    if (!gh_safe_read_u64((unsigned long)pmd, &pmd_val) || !pmd_val ||
        (pmd_val & 0x3UL) != GH_PXD_TYPE_TABLE)
        return;

    pte_table = (u64 *)gh_pxd_page_vaddr(pmd_val);
    pte_kva = gh_phys_to_virt(pmd_val & 0x0000FFFFFFFFF000UL);
    if (ghostmem_table_page_empty(pte_table)) {
        /* 释放空 PTE 表页，清空 PMD 槽位 */
        kfunc_free_pages(pte_kva, 0);
        *pmd = 0;
        ghostmem_tlbi_broadcast(va);
        if (gh_page_level == 4) {
            /* 检查 PMD 表页是否已空 */
            pmd_kva = gh_phys_to_virt(pud_val & 0x0000FFFFFFFFF000UL);
            if (ghostmem_table_page_empty((const u64 *)pmd_kva)) {
                kfunc_free_pages(pmd_kva, 0);
                *pud = 0;
                ghostmem_tlbi_broadcast(va);
                /* 检查 PUD 表页是否已空 */
                pud_kva = gh_phys_to_virt(pgd_val & 0x0000FFFFFFFFF000UL);
                if (ghostmem_table_page_empty((const u64 *)pud_kva)) {
                    kfunc_free_pages(pud_kva, 0);
                    *pgd = 0;
                }
            }
        } else {
            /* 3 级页表：PMD 之上直接是 PGD */
            pmd_kva = gh_phys_to_virt(pgd_val & 0x0000FFFFFFFFF000UL);
            if (ghostmem_table_page_empty((const u64 *)pmd_kva)) {
                kfunc_free_pages(pmd_kva, 0);
                *pgd = 0;
            }
        }
        asm volatile("dsb ish" : : : "memory");
        asm volatile("isb" : : : "memory");
    }
}

void ghostmem_unmap_pages(void *mm, unsigned long va, unsigned long nr_pages)
{
    unsigned long i;
    u64 *ptep;
    unsigned long group_va, end;

    for (i = 0; i < nr_pages; i++) {
        ptep = ghostmem_get_pte(mm, va + i * GHOSTMEM_PAGE_SIZE);
        if (ptep && (*ptep & PTE_VALID))
            *ptep = 0;
    }
    ghostmem_flush_range(va, nr_pages);

    /* 逐 2MB 组回收空表页（块最多跨两组） */
    group_va = va & GH_PTE_COVER_MASK;
    end = va + nr_pages * GHOSTMEM_PAGE_SIZE;
    for (; group_va < end; group_va += GH_PTE_COVER_SIZE)
        ghostmem_reclaim_tables(mm, group_va);
}
