/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem KPM Module - Internal Header
 * Copyright (C) 2024
 */

#ifndef _KPM_GHOSTMEM_INTERNAL_H_
#define _KPM_GHOSTMEM_INTERNAL_H_

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <hook.h>
#include <ksyms.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/rculist.h>
#include <pgtable.h>
#include <asm/current.h>
#include <syscall.h>
#include <kputils.h>
#include <asm/ptrace.h>
#include <asm/atomic.h>
#include <linux/err.h>
#include <predata.h>

#include "ghostmem.h"

/* ========== Kernel function pointers ========== */

/* Memory / task management */
extern void *(*kfunc_find_vma)(void *mm, unsigned long addr);
extern void *(*kfunc_get_task_mm)(void *task);
extern void (*kfunc_mmput)(void *mm);
extern void *kfunc_exit_mmap;              /* hook target for cleanup */

/* RCU (for pid -> task lookup) */
extern void (*kfunc_rcu_read_lock)(void);
extern void (*kfunc_rcu_read_unlock)(void);

/* Page allocation */
extern unsigned long (*kfunc___get_free_pages)(unsigned int gfp_mask, unsigned int order);
extern void (*kfunc_free_pages)(unsigned long addr, unsigned int order);

/* Heap allocation */
extern void *(*kfunc_kzalloc)(size_t size, unsigned int flags);
extern void (*kfunc_kfree)(void *ptr);

/* Safe memory access (kernel addr) */
extern long (*kfunc_copy_from_kernel_nofault)(void *dst, const void *src, size_t size);

/* Address translation */
extern s64 *kvar_memstart_addr;
extern s64 *kvar_physvirt_offset;
extern unsigned long page_offset_base;

/* Page table config (detected from TCR_EL1) */
extern int gh_page_shift;
extern int gh_page_level;

/* Spinlock (framework's _raw_spin_lock via kallsyms) */
extern void (*gh_raw_spin_lock)(raw_spinlock_t *lock);
extern void (*gh_raw_spin_unlock)(raw_spinlock_t *lock);
extern void *(*kfunc_find_task_by_vpid)(pid_t nr);

/*
 * Override framework's spin_lock/spin_unlock to use our gh_raw_* symbols,
 * avoiding undefined references to kf__raw_spin_lock/unlock (wxshadow pattern).
 */
#undef spin_lock
#undef spin_unlock
#undef raw_spin_lock
#undef raw_spin_unlock
#define raw_spin_lock(lock) gh_raw_spin_lock(lock)
#define raw_spin_unlock(lock) gh_raw_spin_unlock(lock)
#define spin_lock(lock) raw_spin_lock(&(lock)->rlock)
#define spin_unlock(lock) raw_spin_unlock(&(lock)->rlock)

/* ========== Page table state ========== */

/* Per-mm ghost block bookkeeping */
struct ghostmem_block {
    struct list_head list;      /* Linked to global block_list */
    void *mm;                   /* Owner mm (struct mm_struct *) */
    unsigned long va;           /* User VA base (page aligned) */
    unsigned long nr_pages;     /* Number of pages in this block */
    unsigned long *pfns;        /* Physical page frames (nr_pages entries) */
};

/* Global list + lock */
extern struct list_head ghostmem_block_list;
extern spinlock_t ghostmem_lock;
extern atomic_t gh_in_flight;      /* in-flight prctl/exit handler counter */

#define GH_HANDLER_ENTER() atomic_inc(&gh_in_flight)
#define GH_HANDLER_EXIT()  atomic_dec(&gh_in_flight)

/* ========== Helpers ========== */

static inline bool gh_is_kva(unsigned long addr)
{
    return (addr >> 48) == 0xffff;
}

/* Safe u64 read from kernel memory */
static inline bool gh_safe_read_u64(unsigned long addr, u64 *out)
{
    if (!gh_is_kva(addr))
        return false;
    if (kfunc_copy_from_kernel_nofault)
        return kfunc_copy_from_kernel_nofault(out, (const void *)addr, sizeof(*out)) == 0;
    *out = *(u64 *)addr;
    return true;
}

/* Address translation (same scheme as wxshadow) */
static inline unsigned long gh_phys_to_virt(unsigned long pa)
{
    if (kvar_physvirt_offset)
        return pa + *kvar_physvirt_offset;
    return (pa - *kvar_memstart_addr) + page_offset_base;
}

static inline unsigned long gh_kaddr_to_phys(unsigned long vaddr)
{
    if (kvar_physvirt_offset)
        return vaddr - *kvar_physvirt_offset;
    return (vaddr - page_offset_base) + *kvar_memstart_addr;
}

static inline unsigned long gh_kaddr_to_pfn(unsigned long vaddr)
{
    return gh_kaddr_to_phys(vaddr) >> GHOSTMEM_PAGE_SHIFT;
}

static inline void *gh_pfn_to_kaddr(unsigned long pfn)
{
    return (void *)gh_phys_to_virt(pfn << GHOSTMEM_PAGE_SHIFT);
}

/* mm->pgd via KP framework's detected pgd_offset */
static inline void *gh_mm_pgd(void *mm)
{
    if (mm_struct_offset.pgd_offset < 0)
        return NULL;
    return *(void **)((char *)mm + mm_struct_offset.pgd_offset);
}

/* ========== Page table index helpers ========== */

static inline unsigned long gh_pgd_index(unsigned long addr)
{
    int pxd_bits = gh_page_shift - 3;
    int pgdir_shift = gh_page_shift + (gh_page_level - 1) * pxd_bits;
    return (addr >> pgdir_shift) & ((1UL << pxd_bits) - 1);
}

static inline unsigned long gh_pud_index(unsigned long addr)
{
    int pxd_bits = gh_page_shift - 3;
    int pud_shift = gh_page_shift + (gh_page_level - 2) * pxd_bits;
    return (addr >> pud_shift) & ((1UL << pxd_bits) - 1);
}

static inline unsigned long gh_pmd_index(unsigned long addr)
{
    int pxd_bits = gh_page_shift - 3;
    int pmd_shift = gh_page_shift + 1 * pxd_bits;
    return (addr >> pmd_shift) & ((1UL << pxd_bits) - 1);
}

static inline unsigned long gh_pte_index(unsigned long addr)
{
    return (addr >> GHOSTMEM_PAGE_SHIFT) & 511;
}

/* Table descriptor types */
#define GH_PXD_TYPE_SECT   0x1UL
#define GH_PXD_TYPE_TABLE  0x3UL

static inline bool gh_pmd_sect(u64 pmd)
{
    return (pmd & 0x3UL) == GH_PXD_TYPE_SECT;
}

static inline bool gh_pmd_table(u64 pmd)
{
    return (pmd & 0x3UL) == GH_PXD_TYPE_TABLE;
}

/* Phys address of next-level table from a descriptor */
static inline unsigned long gh_pxd_page_vaddr(u64 pxd_val)
{
    unsigned long pa = pxd_val & 0x0000FFFFFFFFF000UL;
    return gh_phys_to_virt(pa);
}

/* ========== PTE bits ========== */

#ifndef PTE_VALID
#define PTE_VALID           (1UL << 0)
#endif
#ifndef PTE_TYPE_PAGE
#define PTE_TYPE_PAGE       (3UL << 0)
#endif
#ifndef PTE_USER
#define PTE_USER            (1UL << 6)
#endif
#ifndef PTE_RDONLY
#define PTE_RDONLY          (1UL << 7)
#endif
#ifndef PTE_SHARED
#define PTE_SHARED          (3UL << 8)
#endif
#ifndef PTE_AF
#define PTE_AF              (1UL << 10)
#endif
#ifndef PTE_NG
#define PTE_NG              (1UL << 11)
#endif
#ifndef PTE_UXN
#define PTE_UXN             (1UL << 54)
#endif
#ifndef PTE_ATTRINDX_NORMAL
#define PTE_ATTRINDX_NORMAL (0UL << 2)
#endif

/* ========== pgtable.c interfaces ========== */

/* Create (or reuse) PTE entries for [va, va+nr_pages) and map given pfns.
 * Returns 0 on success.  Only called from process context (prctl handler). */
int ghostmem_map_pages(void *mm, unsigned long va, unsigned long *pfns,
                       unsigned long nr_pages, unsigned int prot);

/* Clear PTE entries for [va, va+nr_pages) and flush TLB. */
void ghostmem_unmap_pages(void *mm, unsigned long va, unsigned long nr_pages);

/* ========== ghostmem.c interfaces ========== */

void *ghostmem_find_block(void *mm, unsigned long va);
int ghostmem_do_alloc(void *mm, unsigned long nr_pages, unsigned int prot,
                      unsigned long *out_va);
int ghostmem_do_free(void *mm, unsigned long va);
int ghostmem_do_info(void *mm, void __user *buf, unsigned long len);
void ghostmem_free_blocks_for_mm(void *mm, const char *reason);
void prctl_before_gh(hook_fargs4_t *args, void *udata);
void exit_mmap_before_gh(hook_fargs1_t *args, void *udata);

/* Symbol + page-config resolution */
unsigned long gh_lookup_name(const char *name);
int ghostmem_resolve_symbols(void);
int ghostmem_detect_page_config(void);

#endif /* _KPM_GHOSTMEM_INTERNAL_H_ */
