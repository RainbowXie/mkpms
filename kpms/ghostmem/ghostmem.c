/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMA-Less Ghost Memory KPM Module - Core
 *
 * Allocates physical pages and hand-builds PTE entries for a target process
 * WITHOUT creating a VMA, so the region is invisible in /proc/<pid>/maps.
 * Exposed via prctl. Lifecycle is tied to exit_mmap and module unload.
 *
 * Copyright (C) 2024
 */

#include "ghostmem.h"

#ifndef __NR_prctl
#define __NR_prctl 167
#endif

KPM_NAME("ghostmem");
KPM_VERSION("0.1.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("ghostmem");
KPM_DESCRIPTION("VMA-Less Ghost Memory Allocator");

/* ========== Global definitions (single TU) ========== */

DEFINE_SPINLOCK(g_lock);
LIST_HEAD(g_blocks);
atomic_t gm_in_flight = ATOMIC_INIT(0);

int gm_page_shift = 12;
int gm_page_level = 0;
int16_t mm_context_id_offset = -1;
unsigned long page_offset_base;
unsigned long gm_base_va = 0x1000000UL;      /* hole scan floor, above mmap_min_addr */

/* Kernel function pointers */
void *(*kfunc_find_vma)(void *mm, unsigned long addr);
void *(*kfunc_get_task_mm)(void *task);
void (*kfunc_mmput)(void *mm);
void *kfunc_exit_mmap;
unsigned long (*kfunc___get_free_pages)(unsigned int gfp_mask, unsigned int order);
void (*kfunc_free_pages)(unsigned long addr, unsigned int order);
void (*kfunc_flush_tlb_page)(void *vma, unsigned long uaddr);
void (*kfunc___flush_tlb_range)(void *vma, unsigned long start,
                                 unsigned long end,
                                 unsigned long stride, bool last_level,
                                 int tlb_level);
void (*kfunc___flush_icache_range)(unsigned long start, unsigned long end);
void *(*kfunc_kzalloc)(size_t size, unsigned int flags);
void (*kfunc_kfree)(void *ptr);
long (*kfunc_copy_to_user)(void __user *to, const void *from, unsigned long n);
void (*kfunc_copy_from_kernel_nofault)(void *dst, const void *src, size_t size);

/* task / spinlock resolved via kallsyms */
struct task_struct *(*gm_find_task_by_vpid)(pid_t nr);
static void (*gm_raw_spin_lock)(raw_spinlock_t *lock);
static void (*gm_raw_spin_unlock)(raw_spinlock_t *lock);

/* Route spin_lock to our resolved symbols (mirrors wxshadow). */
#undef spin_lock
#undef spin_unlock
#define spin_lock(lock) gm_raw_spin_lock(&(lock)->rlock)
#define spin_unlock(lock) gm_raw_spin_unlock(&(lock)->rlock)

/* ========== Symbol resolution ========== */

static int resolve_symbols(void)
{
    u64 tcr, t1sz;
    u64 va_bits;

    pr_info("ghostmem: resolving symbols...\n");

    gm_raw_spin_lock = (void *)lookup_name_safe("_raw_spin_lock");
    gm_raw_spin_unlock = (void *)lookup_name_safe("_raw_spin_unlock");
    gm_find_task_by_vpid = (void *)lookup_name_safe("find_task_by_vpid");
    if (!gm_find_task_by_vpid || !gm_raw_spin_lock || !gm_raw_spin_unlock) {
        pr_err("ghostmem: missing task/spinlock symbols\n");
        return -ESRCH;
    }

    RESOLVE_SYMBOL(get_task_mm);
    RESOLVE_SYMBOL(mmput);
    RESOLVE_SYMBOL(find_vma);
    RESOLVE_SYMBOL(__get_free_pages);
    RESOLVE_SYMBOL(free_pages);
    if (kfunc_kzalloc == NULL) kfunc_kzalloc = (void *)lookup_name_safe("kzalloc");
    if (kfunc_kzalloc == NULL) kfunc_kzalloc = (void *)lookup_name_safe("__kmalloc");
    if (kfunc_kzalloc == NULL) { pr_err("ghostmem: kzalloc missing\n"); return -ESRCH; }
    kfunc_kfree = (void *)lookup_name_safe("kfree");
    if (!kfunc_kfree) { pr_err("ghostmem: kfree missing\n"); return -ESRCH; }

    kfunc_exit_mmap = (void *)lookup_name_safe("exit_mmap");
    if (!kfunc_exit_mmap) {
        pr_err("ghostmem: exit_mmap missing, cannot clean up on process exit\n");
        return -ESRCH;
    }

    kfunc_flush_tlb_page = (void *)lookup_name_safe("flush_tlb_page");
    kfunc___flush_tlb_range = (void *)lookup_name_safe("__flush_tlb_range");
    kfunc___flush_icache_range = (void *)lookup_name_safe("__flush_icache_range");
    kfunc_copy_from_kernel_nofault =
        (void *)lookup_name_safe("copy_from_kernel_nofault");
    kfunc_copy_to_user = (void *)lookup_name_safe("copy_to_user");

    kvar_memstart_addr = (s64 *)lookup_name_safe("memstart_addr");
    kvar_physvirt_offset = (s64 *)lookup_name_safe("physvirt_offset");
    if (!kvar_memstart_addr) {
        pr_err("ghostmem: memstart_addr missing\n");
        return -ESRCH;
    }

    /* Detect page geometry from TCR_EL1 (identical to wxshadow). */
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    t1sz = (tcr >> 16) & 0x3f;
    va_bits = 64 - t1sz;
    {
        u64 tg1 = (tcr >> 30) & 0x3;
        gm_page_shift = 12;
        if (tg1 == 1)
            gm_page_shift = 14;
        else if (tg1 == 3)
            gm_page_shift = 16;
    }
    gm_page_level = (va_bits - 4) / (gm_page_shift - 3);
    page_offset_base = ~0UL << (va_bits - 1);

    /* Detect physvirt offset with an AT round-trip on a real page. */
    {
        unsigned long test = kfunc___get_free_pages(0xcc0, 0);
        if (test) {
            unsigned long real = gm_vaddr_to_paddr(test);
            if (real) {
                detected_physvirt_offset = (s64)test - (s64)real;
                physvirt_offset_valid = 1;
            }
            kfunc_free_pages(test, 0);
        }
    }

    pr_info("ghostmem: page_shift=%d page_level=%d\n",
            gm_page_shift, gm_page_level);
    pr_info("ghostmem: symbols resolved\n");
    return 0;
}

/* ========== alloc ========== */

/*
 * gm_do_alloc - allocate a contiguous power-of-two chunk of physical pages,
 * build user RWX PTEs at a freshly chosen VMA-Less VA, and register a block.
 *
 * order = ceil(log2(nr_pages)); only the first nr_pages of the chunk are used
 * and recorded, the remainder stays reserved. Keeps the block record minimal
 * (base pfn) because free recomputes order from nr_pages.
 */
long gm_do_alloc(pid_t pid, unsigned long nr_pages, unsigned long prot,
                 long *out_va)
{
    void *mm = NULL;
    unsigned long va = 0;
    unsigned long phys = 0;
    unsigned long pfn;
    unsigned int order;
    unsigned long i;
    struct ghostmem_block *blk;
    long ret;

    (void)prot;   /* defaults to RWX per spec; CALLER_RWX not modelled here */

    if (nr_pages == 0 || nr_pages > 0x10000)
        return -EINVAL;

    ret = gm_resolve_pid_to_mm(pid, &mm);
    if (ret < 0)
        return ret;

    va = gm_find_hole_va(mm, nr_pages);
    if (!va) {
        kfunc_mmput(mm);
        return -ENOMEM;
    }

    order = gm_order_of(nr_pages);
    if (order > 9) {                    /* cap chunk at 2MB for sanity */
        kfunc_mmput(mm);
        return -ENOMEM;
    }

    phys = kfunc___get_free_pages(0xcc0, order);
    if (!phys) {
        kfunc_mmput(mm);
        return -ENOMEM;
    }
    pfn = gm_vaddr_to_paddr(phys) >> PAGE_SHIFT;

    for (i = 0; i < nr_pages; i++) {
        int created = 0;
        u64 *ptep = get_or_create_user_pte(mm, va + i * PAGE_SIZE, &created);
        if (!ptep) {
            gm_clear_range_ptes(mm, va, i);
            kfunc_free_pages(phys, order);
            kfunc_mmput(mm);
            return -ENOMEM;
        }
        ghostmem_set_pte(ptep, gm_make_pte(pfn + i, PTE_USER));
    }
    ghostmem_flush_tlb_range(NULL, va, nr_pages);

    blk = kfunc_kzalloc(sizeof(*blk), 0xcc0);
    if (!blk) {
        gm_clear_range_ptes(mm, va, nr_pages);
        kfunc_free_pages(phys, order);
        kfunc_mmput(mm);
        return -ENOMEM;
    }

    blk->mm = mm;
    blk->va = va;
    blk->nr_pages = nr_pages;
    blk->pfn = pfn;
    INIT_LIST_HEAD(&blk->list);

    spin_lock(&g_lock);
    list_add(&blk->list, &g_blocks);
    spin_unlock(&g_lock);

    *out_va = (long)va;
    pr_info("ghostmem: alloc pid=%d va=%lx pages=%lu pfn=%lx\n",
            pid, va, nr_pages, pfn);
    kfunc_mmput(mm);
    return 0;
}

/* ========== free ========== */

/*
 * gm_do_free - free a block owning 'va' (single page in the block or the block
 * itself). Unknown/unregistered VA -> -EINVAL; double free is an error, never a
 * crash, because removal happens only after the physical pages are returned.
 */
long gm_do_free(pid_t pid, unsigned long va)
{
    struct ghostmem_block *blk = NULL;
    struct list_head *pos;
    void *mm = NULL;
    unsigned long base;
    long ret;

    ret = gm_resolve_pid_to_mm(pid, &mm);
    if (ret < 0)
        return ret;

    base = va & PAGE_MASK;

    spin_lock(&g_lock);
    list_for_each(pos, &g_blocks) {
        struct ghostmem_block *b = container_of(pos, struct ghostmem_block, list);
        if (b->mm == mm && base >= b->va &&
            base < b->va + b->nr_pages * PAGE_SIZE) {
            blk = b;
            break;
        }
    }
    if (blk) {
        base = blk->va;
        /* Drop the record only after we have captured the fields. */
        list_del_init(&blk->list);
    }
    spin_unlock(&g_lock);

    if (!blk) {
        kfunc_mmput(mm);
        return -EINVAL;
    }

    gm_clear_range_ptes(mm, base, blk->nr_pages);
    kfunc_free_pages(blk->pfn << PAGE_SHIFT, gm_order_of(blk->nr_pages));

    kfunc_kfree(blk);
    kfunc_mmput(mm);
    pr_info("ghostmem: free va=%lx\n", base);
    return 0;
}

/* ========== info ========== */

long gm_do_info(pid_t pid, void __user *buf, unsigned long len)
{
    struct ghostmem_stats st;
    struct list_head *pos;
    unsigned long pages = 0, blocks = 0, occ = 0;
    void *mm = NULL;
    long ret;

    ret = gm_resolve_pid_to_mm(pid, &mm);
    if (ret < 0)
        return ret;

    spin_lock(&g_lock);
    list_for_each(pos, &g_blocks) {
        struct ghostmem_block *b = container_of(pos, struct ghostmem_block, list);
        if (b->mm != mm)
            continue;
        pages += b->nr_pages;
        blocks++;
        occ += b->nr_pages * PAGE_SIZE;
    }
    spin_unlock(&g_lock);
    kfunc_mmput(mm);

    if (!buf || len < sizeof(st))
        return -EINVAL;

    memset(&st, 0, sizeof(st));
    st.total_pages = pages;
    st.total_blocks = blocks;
    st.occupied = occ;

    if (kfunc_copy_to_user && kfunc_copy_to_user(buf, &st, sizeof(st)) != 0)
        return -EFAULT;

    return 0;
}

/* ========== teardown for exit_mmap / module unload ========== */

/*
 * gm_teardown_all_for_mm - iterative pop-under-lock: clear PTEs, TLB flush,
 * free physical pages and drop the record for every block of 'mm' (or all when
 * mm == NULL). Captures fields under the lock, releases the lock, then acts, so
 * the prctl/free paths racing on unload never double-free a block.
 */
void gm_teardown_all_for_mm(void *mm)
{
    while (1) {
        struct ghostmem_block *blk = NULL;
        void *bmm;
        unsigned long va, nr, pfn;
        struct list_head *pos;

        spin_lock(&g_lock);
        list_for_each(pos, &g_blocks) {
            struct ghostmem_block *b = container_of(pos, struct ghostmem_block, list);
            if (!mm || b->mm == mm) {
                blk = b;
                break;
            }
        }
        if (!blk) {
            spin_unlock(&g_lock);
            break;
        }
        bmm = blk->mm;
        va = blk->va;
        nr = blk->nr_pages;
        pfn = blk->pfn;
        list_del_init(&blk->list);
        spin_unlock(&g_lock);

        gm_clear_range_ptes(bmm, va, nr);
        kfunc_free_pages(pfn << PAGE_SHIFT, gm_order_of(nr));
        kfunc_kfree(blk);
        pr_info("ghostmem: teardown va=%lx pages=%lu\n", va, nr);
    }
}

/* ========== prctl handler ========== */

void prctl_before(hook_fargs4_t *args, void *udata)
{
    int option = (int)syscall_argn(args, 0);
    unsigned long arg2 = syscall_argn(args, 1);
    unsigned long arg3 = syscall_argn(args, 2);
    unsigned long arg4 = syscall_argn(args, 3);
    long ret;

    if (option < PR_GHOSTMEM_ALLOC || option > PR_GHOSTMEM_INFO)
        return;

    GM_HANDLER_ENTER();

    switch (option) {
    case PR_GHOSTMEM_ALLOC: {
        long va = 0;
        ret = gm_do_alloc((pid_t)arg2, arg3, arg4, &va);
        args->ret = ret == 0 ? va : (int)ret;
        args->skip_origin = 1;
        break;
    }
    case PR_GHOSTMEM_FREE:
        ret = gm_do_free((pid_t)arg2, arg3);
        args->ret = (int)ret;
        args->skip_origin = 1;
        break;
    case PR_GHOSTMEM_INFO:
        ret = gm_do_info((pid_t)arg2, (void __user *)arg3, arg4);
        args->ret = (int)ret;
        args->skip_origin = 1;
        break;
    default:
        break;
    }

    GM_HANDLER_EXIT();
}

/* ========== exit_mmap handler ========== */

void exit_mmap_before(hook_fargs1_t *args, void *udata)
{
    void *mm = (void *)args->arg0;

    if (!mm)
        return;
    GM_HANDLER_ENTER();
    gm_teardown_all_for_mm(mm);
    GM_HANDLER_EXIT();
}

/* ========== module init/exit ========== */

static long ghostmem_init(const char *args, const char *event, void __user *reserved)
{
    int ret;

    pr_info("ghostmem: initializing...\n");

    ret = resolve_symbols();
    if (ret < 0)
        return ret;

    if (gm_page_level != 4) {
        pr_err("ghostmem: unsupported page-table geometry (level=%d)\n",
               gm_page_level);
        return -ENOTSUPP;
    }

    ret = hook_syscalln(__NR_prctl, 5, prctl_before, NULL, NULL);
    if (ret != HOOK_NO_ERR) {
        pr_err("ghostmem: hook prctl failed: %d\n", ret);
        return -1;
    }

    ret = hook_wrap1(kfunc_exit_mmap, exit_mmap_before, NULL, NULL);
    if (ret != HOOK_NO_ERR) {
        unhook_syscalln(__NR_prctl, prctl_before, NULL);
        pr_err("ghostmem: hook exit_mmap failed: %d\n", ret);
        return -1;
    }

    pr_info("ghostmem: loaded (prctl 0x%x/0x%x/0x%x)\n",
            PR_GHOSTMEM_ALLOC, PR_GHOSTMEM_FREE, PR_GHOSTMEM_INFO);
    return 0;
}

static long ghostmem_exit(void *__user reserved)
{
    pr_info("ghostmem: unloading...\n");

    /* Block new user ops first, then release every block, then unhook
     * exit_mmap. This ordering keeps teardown racing with live exits safe. */
    unhook_syscalln(__NR_prctl, prctl_before, NULL);

    gm_teardown_all_for_mm(NULL);

    hook_unwrap(kfunc_exit_mmap, exit_mmap_before, NULL);
    pr_info("ghostmem: unloaded\n");
    return 0;
}

KPM_INIT(ghostmem_init);
KPM_EXIT(ghostmem_exit);
