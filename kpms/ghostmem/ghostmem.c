/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem KPM Module - Core
 *
 * VMA-Less 幽灵内存分配器：为指定进程在内核态分配物理页并手动构建 PTE，
 * 不创建任何 VMA，使内存在 /proc/<pid>/maps 中不可见。用户态通过
 * prctl(PR_GHOSTMEM_*, pid, ...) 使用，是 stealth trampoline / 自定义 Linker
 * 的内存底座（见 docs/PLAN.md）。
 *
 * Copyright (C) 2024
 */

#include "ghostmem_internal.h"

/* ========== Global state ========== */

struct list_head ghostmem_block_list = LIST_HEAD_INIT(ghostmem_block_list);
DEFINE_SPINLOCK(ghostmem_lock);
atomic_t gh_in_flight = ATOMIC_INIT(0);

/* ========== Kernel function pointers ========== */

void *(*kfunc_find_vma)(void *mm, unsigned long addr);
void *(*kfunc_get_task_mm)(void *task);
void (*kfunc_mmput)(void *mm);
void *kfunc_exit_mmap = NULL;

void (*kfunc_rcu_read_lock)(void);
void (*kfunc_rcu_read_unlock)(void);

unsigned long (*kfunc___get_free_pages)(unsigned int gfp_mask, unsigned int order);
void (*kfunc_free_pages)(unsigned long addr, unsigned int order);

void *(*kfunc_kzalloc)(size_t size, unsigned int flags);
void (*kfunc_kfree)(void *ptr);

long (*kfunc_copy_from_kernel_nofault)(void *dst, const void *src, size_t size);

s64 *kvar_memstart_addr;
s64 *kvar_physvirt_offset;
unsigned long page_offset_base;

int gh_page_shift;
int gh_page_level;

void (*gh_raw_spin_lock)(raw_spinlock_t *lock);
void (*gh_raw_spin_unlock)(raw_spinlock_t *lock);
void *(*kfunc_find_task_by_vpid)(pid_t nr);

int ghostmem_detect_page_config(void)
{
    u64 tcr_el1;
    u64 t1sz, va_bits, tg1;

    asm volatile("mrs %0, tcr_el1" : "=r"(tcr_el1));
    t1sz = (tcr_el1 >> 16) & 0x3f;
    va_bits = 64 - t1sz;
    tg1 = (tcr_el1 >> 30) & 0x3;

    gh_page_shift = 12;
    if (tg1 == 1)
        gh_page_shift = 14;
    else if (tg1 == 3)
        gh_page_shift = 16;
    gh_page_level = (va_bits - 4) / (gh_page_shift - 3);

    /* 计算用户地址空间上限 TASK_SIZE = 1UL << va_bits */
    page_offset_base = ~0UL << (va_bits - 1);
    pr_info("ghostmem: va_bits=%lld page_shift=%d page_level=%d\n",
            va_bits, gh_page_shift, gh_page_level);
    return 0;
}

/* ========== Lock wrappers ========== */

static inline void gh_lock(void)
{
    spin_lock(&ghostmem_lock);
}

static inline void gh_unlock(void)
{
    spin_unlock(&ghostmem_lock);
}

/* ========== pid -> mm ========== */

/* Resolve pid to mm_struct, refcount held (caller must kfunc_mmput). */
static void *gh_resolve_pid_to_mm(pid_t pid)
{
    void *mm;

    if (pid == 0)
        return kfunc_get_task_mm(current);

    kfunc_rcu_read_lock();
    {
        void *task = kfunc_find_task_by_vpid(pid);
        if (!task) {
            kfunc_rcu_read_unlock();
            return NULL;
        }
        mm = kfunc_get_task_mm(task);
    }
    kfunc_rcu_read_unlock();
    return mm;
}

/* ========== VMA hole finding ========== */

#define VMA_VM_START_OFFSET 0x00
#define VMA_VM_END_OFFSET   0x08

/* Scan base/limit: 落在堆顶与 mmap 区之间的大空洞内（48-bit VA 下远离两端） */
#define GHOSTMEM_SCAN_BASE  0x100000000UL      /* 4GB */
#define GHOSTMEM_SCAN_LIMIT 0x7000000000UL     /* 448GB */

/*
 * Find a VMA-less gap of @size bytes for @mm.
 * 用 find_vma 从低到高跳过已占用区域；find_vma(mm, addr) 返回第一个
 * vm_start >= addr 的 VMA（或 NULL 表示 addr 之上无映射）。
 */
static unsigned long gh_find_hole(void *mm, unsigned long size)
{
    unsigned long addr = GHOSTMEM_SCAN_BASE;
    unsigned long vstart, vend;
    void *vma;

    size = (size + GHOSTMEM_PAGE_SIZE - 1) & GHOSTMEM_PAGE_MASK;

    while (addr + size <= GHOSTMEM_SCAN_LIMIT) {
        vma = kfunc_find_vma(mm, addr);
        if (!vma)
            return addr; /* 之上无映射，直接可用 */

        vstart = *(unsigned long *)((char *)vma + VMA_VM_START_OFFSET);
        vend = *(unsigned long *)((char *)vma + VMA_VM_END_OFFSET);

        if (vstart > addr) {
            /* [addr, vstart) 是空洞 */
            if (vstart - addr >= size)
                return addr;
        }
        addr = (vend + GHOSTMEM_PAGE_SIZE - 1) & GHOSTMEM_PAGE_MASK;
    }
    return 0;
}

/* ========== Block management ========== */

/*
 * 解除 block 映射并释放物理页/块结构（调用时不持锁）。
 * 被 do_free / exit_mmap / 模块卸载共用。
 */
static void ghostmem_release_block(struct ghostmem_block *b)
{
    unsigned long i;

    if (!b)
        return;
    ghostmem_unmap_pages(b->mm, b->va, b->nr_pages);
    for (i = 0; i < b->nr_pages; i++) {
        if (b->pfns[i])
            kfunc_free_pages((unsigned long)gh_pfn_to_kaddr(b->pfns[i]), 0);
    }
    kfunc_kfree(b->pfns);
    kfunc_kfree(b);
}

/* 释放指定 mm（或 mm==NULL 表示全部）的幽灵块（exit_mmap / 卸载用）。
 * 锁内只摘链，释放放到锁外（free_pages 不应在持锁下执行，wxshadow 同款模式）。
 * 退出/卸载场景下 prctl 已摘，块集合稳定，故两段式（计数→收集）无竞态。 */
void ghostmem_free_blocks_for_mm(void *mm, const char *reason)
{
    struct ghostmem_block **arr = NULL;
    struct ghostmem_block *b, *tmp;
    int count = 0, i;

    gh_lock();
    list_for_each_entry(b, &ghostmem_block_list, list) {
        if (!mm || b->mm == mm)
            count++;
    }
    gh_unlock();
    if (count == 0)
        return;

    arr = kfunc_kzalloc(count * sizeof(*arr), 0xcc0);
    if (!arr) {
        pr_err("ghostmem: [%s] OOM collecting blocks (mm=%px)\n", reason, mm);
        return;
    }

    gh_lock();
    i = 0;
    list_for_each_entry_safe(b, tmp, &ghostmem_block_list, list) {
        if ((!mm || b->mm == mm) && i < count) {
            list_del_init(&b->list);
            arr[i++] = b;
        }
    }
    gh_unlock();

    for (i = 0; i < count; i++)
        ghostmem_release_block(arr[i]);
    kfunc_kfree(arr);
    pr_info("ghostmem: [%s] released %d block(s) (mm=%px)\n", reason, count, mm);
}

/* ========== prctl operations ========== */

int ghostmem_do_alloc(void *mm, unsigned long nr_pages, unsigned int prot,
                      unsigned long *out_va)
{
    struct ghostmem_block *b;
    unsigned long va, size, kva, i;
    int ret = 0;

    if (nr_pages == 0 || nr_pages > GHOSTMEM_MAX_PAGES)
        return -EINVAL;

    size = nr_pages * GHOSTMEM_PAGE_SIZE;
    va = gh_find_hole(mm, size);
    if (!va) {
        pr_err("ghostmem: no VMA hole for %lu pages\n", nr_pages);
        return -ENOMEM;
    }

    b = kfunc_kzalloc(sizeof(*b), 0xcc0);
    if (!b)
        return -ENOMEM;
    b->pfns = kfunc_kzalloc(nr_pages * sizeof(unsigned long), 0xcc0);
    if (!b->pfns) {
        kfunc_kfree(b);
        return -ENOMEM;
    }

    /* 逐页分配物理页 */
    for (i = 0; i < nr_pages; i++) {
        kva = kfunc___get_free_pages(0xcc0, 0);
        if (!kva) {
            pr_err("ghostmem: page alloc failed at %lu/%lu\n", i, nr_pages);
            ret = -ENOMEM;
            goto err;
        }
        b->pfns[i] = gh_kaddr_to_pfn(kva);
    }

    /* 手动构建 PTE（VMA-Less 映射） */
    ret = ghostmem_map_pages(mm, va, b->pfns, nr_pages, prot);
    if (ret < 0) {
        pr_err("ghostmem: map pages failed: %d\n", ret);
        goto err;
    }

    b->mm = mm;
    b->va = va;
    b->nr_pages = nr_pages;

    gh_lock();
    list_add_tail(&b->list, &ghostmem_block_list);
    gh_unlock();

    *out_va = va;
    pr_info("ghostmem: alloc %lu page(s) at 0x%lx for mm=%px prot=0x%x\n",
            nr_pages, va, mm, prot);
    return 0;

err:
    for (i = 0; i < nr_pages; i++) {
        if (b->pfns[i])
            kfunc_free_pages((unsigned long)gh_pfn_to_kaddr(b->pfns[i]), 0);
    }
    kfunc_kfree(b->pfns);
    kfunc_kfree(b);
    return ret;
}

int ghostmem_do_free(void *mm, unsigned long va)
{
    struct ghostmem_block *b;

    gh_lock();
    list_for_each_entry(b, &ghostmem_block_list, list) {
        if (b->mm == mm && b->va == va) {
            list_del_init(&b->list);
            gh_unlock();
            ghostmem_release_block(b);
            pr_info("ghostmem: freed block at 0x%lx for mm=%px\n", va, mm);
            return 0;
        }
    }
    gh_unlock();
    return -EINVAL; /* 未登记地址 */
}

int ghostmem_do_info(void *mm, void __user *buf, unsigned long len)
{
    struct ghostmem_block *b;
    struct ghostmem_stats stats = { 0, 0 };

    gh_lock();
    list_for_each_entry(b, &ghostmem_block_list, list) {
        if (b->mm == mm) {
            stats.nr_blocks++;
            stats.nr_pages += b->nr_pages;
        }
    }
    gh_unlock();

    if (!buf || len < sizeof(stats))
        return -EINVAL;
    return compat_copy_to_user(buf, &stats, sizeof(stats));
}

/* ========== prctl hook ========== */

void prctl_before_gh(hook_fargs4_t *args, void *udata)
{
    int option = (int)syscall_argn(args, 0);
    unsigned long arg2 = syscall_argn(args, 1);
    unsigned long arg3 = syscall_argn(args, 2);
    unsigned long arg4 = syscall_argn(args, 3);
    void *mm;
    unsigned long va;
    int ret;
    pid_t pid;

    if (option < PR_GHOSTMEM_ALLOC || option > PR_GHOSTMEM_INFO)
        return;

    GH_HANDLER_ENTER();

    switch (option) {
    case PR_GHOSTMEM_ALLOC:
        pid = (pid_t)arg2;
        mm = gh_resolve_pid_to_mm(pid);
        if (!mm) { args->ret = -ESRCH; args->skip_origin = 1; break; }
        ret = ghostmem_do_alloc(mm, arg3, (unsigned int)arg4, &va);
        kfunc_mmput(mm);
        args->ret = ret ? ret : (long)va;
        args->skip_origin = 1;
        break;

    case PR_GHOSTMEM_FREE:
        pid = (pid_t)arg2;
        mm = gh_resolve_pid_to_mm(pid);
        if (!mm) { args->ret = -ESRCH; args->skip_origin = 1; break; }
        ret = ghostmem_do_free(mm, arg3);
        kfunc_mmput(mm);
        args->ret = ret;
        args->skip_origin = 1;
        break;

    case PR_GHOSTMEM_INFO:
        pid = (pid_t)arg2;
        mm = gh_resolve_pid_to_mm(pid);
        if (!mm) { args->ret = -ESRCH; args->skip_origin = 1; break; }
        ret = ghostmem_do_info(mm, (void __user *)arg3, arg4);
        kfunc_mmput(mm);
        args->ret = ret;
        args->skip_origin = 1;
        break;

    default:
        break;
    }

    GH_HANDLER_EXIT();
}

/* ========== exit_mmap hook ========== */

void exit_mmap_before_gh(hook_fargs1_t *args, void *udata)
{
    void *mm = (void *)args->arg0;

    if (!mm)
        return;
    GH_HANDLER_ENTER();
    ghostmem_free_blocks_for_mm(mm, "exit_mmap");
    GH_HANDLER_EXIT();
}

/* ========== Module init/exit ========== */

static long ghostmem_init(const char *args, const char *event, void *__user reserved)
{
    int ret;

    pr_info("ghostmem: initializing...\n");

    ret = ghostmem_resolve_symbols();
    if (ret < 0)
        return ret;

    ret = ghostmem_detect_page_config();
    if (ret < 0)
        return ret;

    ret = hook_syscalln(__NR_prctl, 5, prctl_before_gh, NULL, NULL);
    if (ret != HOOK_NO_ERR) {
        pr_err("ghostmem: failed to hook prctl: %d\n", ret);
        return -1;
    }
    pr_info("ghostmem: hooked prctl syscall\n");

    ret = hook_wrap1(kfunc_exit_mmap, exit_mmap_before_gh, NULL, NULL);
    if (ret != HOOK_NO_ERR) {
        pr_err("ghostmem: failed to hook exit_mmap: %d\n", ret);
        unhook_syscalln(__NR_prctl, prctl_before_gh, NULL);
        return -1;
    }
    pr_info("ghostmem: hooked exit_mmap for cleanup\n");

    pr_info("ghostmem: module loaded (prctl 0x%x/0x%x/0x%x)\n",
            PR_GHOSTMEM_ALLOC, PR_GHOSTMEM_FREE, PR_GHOSTMEM_INFO);
    return 0;
}

static long ghostmem_exit(void *__user reserved)
{
    pr_info("ghostmem: unloading...\n");

    /* Phase 1: 先摘 prctl，阻止新操作 */
    unhook_syscalln(__NR_prctl, prctl_before_gh, NULL);

    /* Phase 2: 释放全部遗留块（exit_mmap hook 仍在线，处理并发退出） */
    ghostmem_free_blocks_for_mm(NULL, "module unload");

    /* Phase 3: 最后摘 exit_mmap */
    if (kfunc_exit_mmap)
        hook_unwrap(kfunc_exit_mmap, exit_mmap_before_gh, NULL);

    pr_info("ghostmem: module unloaded\n");
    return 0;
}

KPM_NAME("ghostmem");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("ethan");
KPM_DESCRIPTION("VMA-Less Ghost Memory - invisible RWX allocations via manual PTE");
KPM_INIT(ghostmem_init);
KPM_EXIT(ghostmem_exit);
