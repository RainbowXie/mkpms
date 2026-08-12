/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMA-Less Ghost Memory KPM Module - Internal Header
 *
 * Framework includes, prctl constants, kernel function pointers and the shared
 * declarations used by ghostmem.c and ghostmem_pgtable.c. Kept to one header
 * so the module stays a 4-file deliverable (no separate *_internal.h).
 *
 * Copyright (C) 2024
 */

#ifndef _KPM_GHOSTMEM_H_
#define _KPM_GHOSTMEM_H_

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
#include <asm/atomic.h>
#include <linux/err.h>

/* prctl options - GM = GhostMem (unique constants, never collide with clib) */
#define PR_GHOSTMEM_ALLOC       0x47474d01  /* prctl(opt, pid, nr_pages, prot, 0) -> user VA */
#define PR_GHOSTMEM_FREE        0x47474d02  /* prctl(opt, pid, va, 0, 0) */
#define PR_GHOSTMEM_INFO        0x47474d03  /* prctl(opt, pid, buf, len, 0) */

#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* PTE bits (fallback definitions if pgtable.h lacks them) */
#ifndef PTE_VALID
#define PTE_VALID           (1UL << 0)
#endif
#ifndef PTE_TYPE_PAGE
#define PTE_TYPE_PAGE       (3UL << 0)
#endif
#ifndef PTE_USER
#define PTE_USER            (1UL << 6)
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
#ifndef PTE_ATTRINDX_NORMAL
#define PTE_ATTRINDX_NORMAL (0UL << 2)
#endif

/* Statistics written to the INFO user buffer. Fixed external layout. */
struct ghostmem_stats {
    unsigned long total_pages;
    unsigned long total_blocks;
    unsigned long occupied;
};

/*
 * Per-block record on the global list. Base tuple (mm, va, nr_pages, pfn) per
 * spec. The physical run is always a power-of-two (2^order) allocation so
 * 'order' is derived from nr_pages at free time (no dedicated field).
 */
struct ghostmem_block {
    struct list_head list;
    void *mm;
    unsigned long va;
    unsigned long nr_pages;
    unsigned long pfn;
};

/* ========== Kernel function pointers ========== */

extern void *(*kfunc_find_vma)(void *mm, unsigned long addr);
extern void *(*kfunc_get_task_mm)(void *task);
extern void (*kfunc_mmput)(void *mm);
extern void *kfunc_exit_mmap;
extern unsigned long (*kfunc___get_free_pages)(unsigned int gfp_mask, unsigned int order);
extern void (*kfunc_free_pages)(unsigned long addr, unsigned int order);
extern void (*kfunc_flush_tlb_page)(void *vma, unsigned long uaddr);
extern void (*kfunc___flush_tlb_range)(void *vma, unsigned long start,
                                        unsigned long end,
                                        unsigned long stride, bool last_level,
                                        int tlb_level);
extern void (*kfunc___flush_icache_range)(unsigned long start, unsigned long end);

/* mm->pgd and user-page helpers */
extern int16_t mm_context_id_offset;
extern unsigned long page_offset_base;
extern int gm_page_shift;
extern int gm_page_level;
extern unsigned long gm_base_va;

/* Address translation (defined in ghostmem_pgtable.c) */
extern unsigned long phys_to_virt_safe_pa(unsigned long pa);
extern unsigned long gm_vaddr_to_paddr(unsigned long vaddr);
extern s64 *kvar_memstart_addr;
extern s64 *kvar_physvirt_offset;
extern s64 detected_physvirt_offset;
extern int physvirt_offset_valid;

/* Safe symbol lookup (defined in ghostmem_pgtable.c) */
extern struct task_struct *gm_find_task_by_vpid;
extern unsigned long lookup_name_safe(const char *name);

/*
 * RESOLVE_SYMBOL - resolve a kfunc name via the vmlinux-only lookup and bail
 * with -ESRCH (missing symbol) if it is absent. Only used by ghostmem.c's
 * resolve_symbols(), but kept here so ghostmem.c stays under the line cap.
 */
#define RESOLVE_SYMBOL(name) \
    do { \
        kfunc_##name = (typeof(kfunc_##name))lookup_name_safe(#name); \
        if (!kfunc_##name) { \
            pr_err("ghostmem: missing symbol %s\n", #name); \
            return -ESRCH; \
        } \
    } while (0)

/*
 * gm_order_of - smallest order whose 2^order >= n. Blocks are power-of-two
 * physical allocations, so alloc/free/teardown all derive the order back from
 * the recorded nr_pages. Avoids three identical while-loops in ghostmem.c.
 */
static inline unsigned int gm_order_of(unsigned long n)
{
    unsigned int order = 0;
    while ((1UL << order) < n)
        order++;
    return order;
}

/* ========== Global state ========== */

extern struct list_head g_blocks;   /* list of struct ghostmem_block */
extern spinlock_t g_lock;

extern atomic_t gm_in_flight;
#define GM_HANDLER_ENTER() atomic_inc(&gm_in_flight)
#define GM_HANDLER_EXIT()  atomic_dec(&gm_in_flight)

/* ========== Core functions ========== */

int gm_resolve_pid_to_mm(pid_t pid, void **mm);
long gm_do_free(pid_t pid, unsigned long va);
long gm_do_info(pid_t pid, void __user *buf, unsigned long len);
void gm_teardown_all_for_mm(void *mm);
void prctl_before(hook_fargs4_t *args, void *udata);
void exit_mmap_before(hook_fargs1_t *args, void *udata);

/* ========== Page-table functions (ghostmem_pgtable.c) ========== */

u64 *get_user_pte(void *mm, unsigned long addr, void **ptlp);
u64 *get_or_create_user_pte(void *mm, unsigned long addr, int *created);
void ghostmem_set_pte(u64 *ptep, u64 pte);
u64 gm_make_pte(unsigned long pfn, u64 prot);
void ghostmem_flush_tlb_page(void *vma, unsigned long uaddr);
void ghostmem_flush_tlb_range(void *vma, unsigned long start,
                              unsigned long nr_pages);
unsigned long gm_find_hole_va(void *mm, unsigned long nr_pages);
void gm_clear_range_ptes(void *mm, unsigned long va, unsigned long nr);

#endif /* _KPM_GHOSTMEM_H_ */
