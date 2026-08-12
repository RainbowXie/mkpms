/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMA-Less Ghost Memory KPM Module - Page Table Operations
 *
 * Manual user page-table walk, missing-level creation, raw PTE writes and TLB
 * invalidation. Mirrors wxshadow_pgtable.c for ARM64 but adds
 * get_or_create_user_pte(): a VMA-Less hole has no VMA so the covering
 * intermediate tables are usually not allocated yet and must be created here.
 *
 * Copyright (C) 2024
 */

#include "ghostmem.h"

/* ========== Safe symbol lookup (vmlinux only, no module traversal) ========== */

struct gm_lookup_data {
    const char *name;
    unsigned long addr;
};

static int gm_lookup_callback(void *data, const char *name, struct module *mod,
                              unsigned long addr)
{
    struct gm_lookup_data *ld = data;

    if (strcmp(name, ld->name) == 0) {
        ld->addr = addr;
        return 1;
    }
    return 0;
}

unsigned long lookup_name_safe(const char *name)
{
    struct gm_lookup_data ld = { .name = name, .addr = 0 };

    if (kallsyms_on_each_symbol)
        kallsyms_on_each_symbol(gm_lookup_callback, &ld);
    return ld.addr;
}

/* ========== Address translation ========== */

s64 *kvar_memstart_addr;
s64 *kvar_physvirt_offset;
s64 detected_physvirt_offset;
int physvirt_offset_valid;

unsigned long phys_to_virt_safe_pa(unsigned long pa)
{
    if (physvirt_offset_valid)
        return pa + (unsigned long)detected_physvirt_offset;
    if (kvar_physvirt_offset)
        return pa + *kvar_physvirt_offset;
    return (pa - *kvar_memstart_addr) + page_offset_base;
}

unsigned long gm_vaddr_to_paddr(unsigned long vaddr)
{
    if (physvirt_offset_valid)
        return vaddr - (unsigned long)detected_physvirt_offset;
    if (kvar_physvirt_offset)
        return vaddr - *kvar_physvirt_offset;
    return (vaddr - page_offset_base) + *kvar_memstart_addr;
}

/* ========== Index arithmetic (ARM64 page tables) ========== */

static inline int gm_pxd_bits(void) { return gm_page_shift - 3; }

static inline unsigned long gm_pgd_index(unsigned long addr)
{
    int shift = gm_page_shift + (gm_page_level - 1) * gm_pxd_bits();
    return (addr >> shift) & ((1UL << gm_pxd_bits()) - 1);
}

static inline unsigned long gm_pud_index(unsigned long addr)
{
    int shift = gm_page_shift + (gm_page_level - 2) * gm_pxd_bits();
    return (addr >> shift) & ((1UL << gm_pxd_bits()) - 1);
}

static inline unsigned long gm_pmd_index(unsigned long addr)
{
    int shift = gm_page_shift + 1 * gm_pxd_bits();
    return (addr >> shift) & ((1UL << gm_pxd_bits()) - 1);
}

static inline unsigned long gm_pte_index(unsigned long addr)
{
    return (addr >> PAGE_SHIFT) & ((1UL << gm_pxd_bits()) - 1);
}

/* ========== Descriptor helpers ========== */

#define GM_DESC_MASK   0x3UL
#define GM_DESC_TABLE  0x3UL

static bool gm_is_kva(unsigned long addr)
{
    return (addr >> 48) == 0xffff;
}

static inline unsigned long gm_desc_pa(u64 desc)
{
    return desc & 0x0000FFFFFFFFF000UL;
}

static bool gm_is_table(u64 desc)
{
    return (desc & GM_DESC_MASK) == GM_DESC_TABLE;
}

static inline u64 *gm_table_vaddr(u64 desc)
{
    unsigned long pa = gm_desc_pa(desc);

    if (!pa)
        return NULL;
    return (u64 *)phys_to_virt_safe_pa(pa);
}

static inline void *gm_pgd_of(void *mm)
{
    if (mm_struct_offset.pgd_offset < 0)
        return NULL;
    return *(void **)((char *)mm + mm_struct_offset.pgd_offset);
}

/*
 * Allocate a fresh zeroed single-page descriptor table.
 * 0xcc0 == GFP_KERNEL|__GFP_ZERO (matches wxshadow page alloc usage).
 */
static u64 *gm_alloc_pt_page(void)
{
    unsigned long addr;

    if (!kfunc___get_free_pages)
        return NULL;
    addr = kfunc___get_free_pages(0xcc0, 0);
    if (!addr || !gm_is_kva(addr))
        return NULL;
    memset((void *)addr, 0, PAGE_SIZE);
    return (u64 *)addr;
}

static void gm_free_pt_page(u64 *page)
{
    if (kfunc_free_pages && page)
        kfunc_free_pages((unsigned long)page, 0);
}

/* Store a zeroed child-table descriptor into 'slot' (phys addr + TABLE type). */
static void gm_install_table(u64 *slot, u64 *table)
{
    *(volatile u64 *)slot =
        ((u64)table - page_offset_base) | GM_DESC_TABLE;
}

/*
 * get_or_create_user_pte - PTE slot for 'addr' in 'mm', creating missing
 * intermediate tables when needed. Only the common 4-level config is built
 * (the scan in ghostmem.c confirms gm_page_level == 4); folded 3-level setups
 * abort rather than build an ambiguous chain.
 *
 * Returns NULL on failure. On success *created counts table pages allocated.
 */
u64 *get_or_create_user_pte(void *mm, unsigned long addr, int *created)
{
    u64 *pgd, *pud, *pmd;
    u64 val;

    if (created)
        *created = 0;
    if (gm_page_level != 4)
        return NULL;

    pgd = (u64 *)gm_pgd_of(mm);
    if (!pgd || !gm_is_kva((unsigned long)pgd))
        return NULL;
    pgd += gm_pgd_index(addr);

    val = *(volatile u64 *)pgd;
    if (!gm_is_table(val)) {
        pud = gm_alloc_pt_page();
        if (!pud)
            return NULL;
        gm_install_table(pgd, pud);
        if (created)
            (*created)++;
        val = *(volatile u64 *)pgd;
    }
    pud = gm_table_vaddr(val) + gm_pud_index(addr);
    if (!gm_is_kva((unsigned long)pud))
        return NULL;

    val = *(volatile u64 *)pud;
    if (!gm_is_table(val)) {
        pmd = gm_alloc_pt_page();
        if (!pmd)
            return NULL;
        gm_install_table(pud, pmd);
        if (created)
            (*created)++;
        val = *(volatile u64 *)pud;
    }
    pmd = gm_table_vaddr(val) + gm_pmd_index(addr);
    if (!gm_is_kva((unsigned long)pmd))
        return NULL;

    val = *(volatile u64 *)pmd;
    if (!gm_is_table(val)) {
        u64 *pte = gm_alloc_pt_page();
        if (!pte)
            return NULL;
        gm_install_table(pmd, pte);
        if (created)
            (*created)++;
        val = *(volatile u64 *)pmd;
    }

    {
        u64 *ptep = gm_table_vaddr(val) + gm_pte_index(addr);
        if (!gm_is_kva((unsigned long)ptep))
            return NULL;
        return ptep;
    }
}

/*
 * get_user_pte - read-only PTE lookup; requires the whole chain to already
 * exist. Used for FREE validation and stats (guaranteed by a prior ALLOC).
 */
u64 *get_user_pte(void *mm, unsigned long addr, void **ptlp)
{
    u64 *pgd, *pud, *pmd;
    u64 val;

    if (ptlp)
        *ptlp = NULL;

    pgd = (u64 *)gm_pgd_of(mm);
    if (!pgd || !gm_is_kva((unsigned long)pgd))
        return NULL;
    val = *(volatile u64 *)(pgd + gm_pgd_index(addr));
    if (!gm_is_table(val))
        return NULL;

    if (gm_page_level == 4) {
        pud = gm_table_vaddr(val) + gm_pud_index(addr);
        if (!gm_is_kva((unsigned long)pud))
            return NULL;
        val = *(volatile u64 *)pud;
        if (!gm_is_table(val))
            return NULL;
    }
    pmd = gm_table_vaddr(val) + gm_pmd_index(addr);
    if (!gm_is_kva((unsigned long)pmd))
        return NULL;
    val = *(volatile u64 *)pmd;
    if (!gm_is_table(val))
        return NULL;

    {
        u64 *ptep = gm_table_vaddr(val) + gm_pte_index(addr);
        if (!gm_is_kva((unsigned long)ptep))
            return NULL;
        return ptep;
    }
}

/*
 * set_pte_at_raw - raw volatile PTE store. Must be paired with a TLB flush
 * before the guest touches the address; the caller flushes the whole range
 * once, after every PTE in the run is laid down.
 */
void ghostmem_set_pte(u64 *ptep, u64 pte)
{
    *(volatile u64 *)ptep = pte;
}

/* Standard user RWX PTE for a PFN (prot add PTE_USER; W/UXN default per spec). */
u64 gm_make_pte(unsigned long pfn, u64 prot)
{
    return (pfn << PAGE_SHIFT) | prot | PTE_VALID | PTE_TYPE_PAGE |
           PTE_AF | PTE_SHARED | PTE_NG | PTE_ATTRINDX_NORMAL;
}

/*
 * ghostmem_flush_tlb_page - invalidate one user VA, 3-tier like wxshadow.
 */
void ghostmem_flush_tlb_page(void *vma, unsigned long uaddr)
{
    if (kfunc_flush_tlb_page) {
        kfunc_flush_tlb_page(vma, uaddr);
    } else if (kfunc___flush_tlb_range) {
        kfunc___flush_tlb_range(vma, uaddr, uaddr + PAGE_SIZE, PAGE_SIZE,
                                true, 3);
    } else {
        /* Last resort: broadcast-invalidate the single VA with raw TLBI. */
        asm volatile("tlbi vaale1is, %0" : : "r"(uaddr >> 12) : "memory");
        asm volatile("dsb ish" : : : "memory");
        asm volatile("isb" : : : "memory");
    }
}

/*
 * ghostmem_flush_tlb_range - invalidate a contiguous run. Prefer one kernel
 * range call when available, otherwise per-page.
 */
void ghostmem_flush_tlb_range(void *vma, unsigned long start,
                              unsigned long nr_pages)
{
    unsigned long end = start + nr_pages * PAGE_SIZE;
    unsigned long a;

    if (kfunc___flush_tlb_range) {
        kfunc___flush_tlb_range(vma, start, end, PAGE_SIZE, true, 3);
    } else if (kfunc_flush_tlb_page) {
        for (a = start; a < end; a += PAGE_SIZE)
            kfunc_flush_tlb_page(vma, a);
    } else {
        for (a = start; a < end; a += PAGE_SIZE)
            ghostmem_flush_tlb_page(vma, a);
    }
}

/* ========== pid -> mm resolution ========== */

/*
 * gm_resolve_pid_to_mm - find the task and its mm for a pid.
 * get_task_mm() takes an mm_users ref; caller must mmput() afterward.
 * Returns -ESRCH on invalid/exited pid or when the task has no user mm.
 */
int gm_resolve_pid_to_mm(pid_t pid, void **mm)
{
    struct task_struct *task;

    if (!gm_find_task_by_vpid)
        return -ESRCH;

    task = gm_find_task_by_vpid(pid);
    if (!task)
        return -ESRCH;

    *mm = kfunc_get_task_mm(task);
    if (!*mm)
        return -ESRCH;
    return 0;
}

/* ========== VMA hole scan ========== */

/*
 * gm_find_hole_va - find a page-aligned run of nr_pages with no covering VMA,
 * scanning upward from gm_base_va. Uses find_vma() to detect overlap and skips
 * past any VMA that matches the leading page so we never re-scan it.
 */
unsigned long gm_find_hole_va(void *mm, unsigned long nr_pages)
{
    unsigned long size = nr_pages * PAGE_SIZE;
    unsigned long cand;

    for (cand = gm_base_va; cand < (1UL << 47); cand += size) {
        void *vma = kfunc_find_vma(mm, cand);

        if (vma) {
            const unsigned long *p = (const unsigned long *)vma;
            unsigned long vend = p[1];          /* vm_end is field 2 */
            if (vend > cand)
                cand = (vend + PAGE_SIZE - 1) & PAGE_MASK;
            continue;
        }

        /* The block tail must also be VMA-free. */
        {
            void *v2 = kfunc_find_vma(mm, cand + size - PAGE_SIZE);
            if (v2) {
                const unsigned long *p = (const unsigned long *)v2;
                unsigned long vend2 = p[1];
                if (vend2 > cand + size - PAGE_SIZE) {
                    cand = (vend2 + PAGE_SIZE - 1) & PAGE_MASK;
                    continue;
                }
            }
        }
        return cand;
    }
    return 0;
}

/*
 * gm_clear_range_ptes - clear the PTEs of a laid-down run (rollback helper).
 * The table pages created by get_or_create_user_pte stay resident; a rare
 * allocation failure leaks only the zeroed table pages, which is acceptable for
 * a failed prctl and far safer than freeing them while other blocks still use
 * the same tables.
 */
void gm_clear_range_ptes(void *mm, unsigned long va, unsigned long nr)
{
    unsigned long i;

    for (i = 0; i < nr; i++) {
        u64 *p = get_user_pte(mm, va + i * PAGE_SIZE, NULL);
        if (p)
            ghostmem_set_pte(p, 0);
    }
    ghostmem_flush_tlb_range(NULL, va, nr);
}
