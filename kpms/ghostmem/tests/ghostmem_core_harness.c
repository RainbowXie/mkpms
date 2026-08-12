/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ghostmem.c 核心逻辑执行级 harness。
 *
 * 覆盖：gh_find_hole（空洞查找/跳过已占用 VMA）、ghostmem_do_alloc
 * （prot==0 默认 RWX 归一化、块登记）、ghostmem_do_free。
 * 桩：模拟 VMA 列表 + 双向链表 + 空 spinlock + 页对齐分配器。
 *
 * 编译运行：
 *   gcc -DGHOSTMEM_CORE_HARNESS -Ikpms/ghostmem -Ikpms/ghostmem/tests/shim \
 *       -o /tmp/core_harness tests/ghostmem_core_harness.c \
 *       kpms/ghostmem/ghostmem.c kpms/ghostmem/ghostmem_pgtable.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _KPM_GHOSTMEM_INTERNAL_H_
#define __user
#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))

#include "ghostmem.h"
#include "ghostmem_args.h"

/* ---- list_head 桩（双向链表） ---- */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define INIT_LIST_HEAD(l) do { (l)->next = (l); (l)->prev = (l); } while (0)
#define list_add_tail(n, h) do { \
    (n)->prev = (h)->prev; (n)->next = (h); \
    (h)->prev->next = (n); (h)->prev = (n); } while (0)
#define list_del_init(l) do { \
    (l)->next->prev = (l)->prev; (l)->prev->next = (l)->next; \
    INIT_LIST_HEAD(l); } while (0)
#define list_for_each_entry(e, h, m) \
    for ((e) = (void *)((h)->next); &(e)->m != (h); (e) = (void *)((e)->m.next))
#define list_for_each_entry_safe(e, t, h, m) \
    for ((e) = (void *)((h)->next), (t) = (void *)((e)->m.next); \
         &(e)->m != (h); (e) = (t), (t) = (void *)((t)->m.next))

/* ---- spinlock 桩（空操作） ---- */
typedef struct { struct { int raw_lock; } rlock; } spinlock_t;
typedef struct { int raw_lock; } raw_spinlock_t;
static void gh_lock_stub(raw_spinlock_t *l) { (void)l; }
static void gh_unlock_stub(raw_spinlock_t *l) { (void)l; }
#define DEFINE_SPINLOCK(x) spinlock_t x = { { 0 } }
/* ghostmem.c 内 spin_lock 宏展开为 gh_raw_spin_lock(&lock->rlock)：
   提供同名函数符号供链接 */
void gh_raw_spin_lock(raw_spinlock_t *l) { (void)l; }
void gh_raw_spin_unlock(raw_spinlock_t *l) { (void)l; }

/* ---- 类型/宏桩 ---- */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int pid_t;
#define ATOMIC_INIT(v) { .counter = (v) }
typedef struct { int counter; } atomic_t;
#define atomic_inc(a) ((a)->counter++)
#define atomic_dec(a) ((a)->counter--)

/* ---- kfunc 桩 ---- */
void *(*kfunc_find_vma)(void *mm, unsigned long addr);
void *(*kfunc_get_task_mm)(void *task);
void (*kfunc_mmput)(void *mm);
void *kfunc_exit_mmap;
unsigned long (*kfunc___get_free_pages)(unsigned int gfp, unsigned int order);
void (*kfunc_free_pages)(unsigned long addr, unsigned int order);
void *(*kfunc_kzalloc)(size_t n, unsigned int f);
void (*kfunc_kfree)(void *p);
long (*kfunc_copy_from_kernel_nofault)(void *d, const void *s, size_t n);
s64 *kvar_memstart_addr;
s64 *kvar_physvirt_offset;
unsigned long page_offset_base;
void *(*kfunc_find_task_by_vpid)(pid_t);
int gh_page_shift = 12;
int gh_page_level = 4;

/* ---- 页对齐分配器（复用 pgtable harness 模式） ---- */
static void *g_raw[256];
static int g_raw_count = 0;
static unsigned long g_pages = 0;
static unsigned long stub_alloc_page(unsigned int gfp, unsigned int order)
{
    (void)gfp;
    size_t sz = (PAGE_SIZE << order) + PAGE_SIZE;
    unsigned char *raw = malloc(sz);
    if (!raw) return 0;
    unsigned long aligned = ((unsigned long)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    memset((void *)aligned, 0, PAGE_SIZE << order);
    if (g_raw_count < 256) g_raw[g_raw_count++] = raw;
    g_pages++;
    return aligned;
}
static void stub_free_page(unsigned long addr, unsigned int order)
{
    (void)order;
    for (int i = 0; i < g_raw_count; i++) {
        if ((unsigned long)g_raw[i] <= addr && (unsigned long)g_raw[i] + 2 * PAGE_SIZE > addr) {
            free(g_raw[i]); g_raw[i] = g_raw[--g_raw_count]; g_pages--; return;
        }
    }
}
static void *g_current_mm = NULL;
static void *stub_gtm(void *t) { (void)t; return g_current_mm; }
static void stub_mmput(void *m) { (void)m; }
static void *stub_ftbv(pid_t p) { (void)p; return NULL; }
/* kfunc 指针变量定义（指向 stub；ghostmem.c 引用同名外部符号） */
void *(*kfunc_get_task_mm)(void *task) = stub_gtm;
void (*kfunc_mmput)(void *mm) = stub_mmput;
void *(*kfunc_find_task_by_vpid)(pid_t nr) = stub_ftbv;
unsigned long (*kfunc___get_free_pages)(unsigned int gfp, unsigned int order) = stub_alloc_page;
void (*kfunc_free_pages)(unsigned long addr, unsigned int order) = stub_free_page;
void *(*kfunc_kzalloc)(size_t n, unsigned int f) = calloc;
void (*kfunc_kfree)(void *p) = free;
long (*kfunc_copy_from_kernel_nofault)(void *d, const void *s, size_t n) = NULL;
s64 *kvar_memstart_addr = NULL;
s64 *kvar_physvirt_offset = NULL;
unsigned long page_offset_base = 0;
/* ---- gh_* 助手（单地址空间） ---- */
#define gh_is_kva(a) ((a) != 0)
#define gh_safe_read_u64(a, o) (*(o) = *(volatile u64 *)(a), 1)
typedef struct { u64 *pgd; } stub_mm;
#define gh_mm_pgd(mm) (((stub_mm *)mm)->pgd)
static inline unsigned long gh_kaddr_to_phys(unsigned long va) { return va; }
static inline unsigned long gh_phys_to_virt(unsigned long pa) { return pa; }
static inline unsigned long gh_kaddr_to_pfn(unsigned long va) { return va >> 12; }
static inline void *gh_pfn_to_kaddr(unsigned long pfn) { return (void *)(pfn << 12); }
static inline unsigned long gh_pgd_index(unsigned long a) { int b = 9, s = 12 + 3 * 9; return (a >> s) & 511; }
static inline unsigned long gh_pud_index(unsigned long a) { int b = 9, s = 12 + 2 * 9; return (a >> s) & 511; }
static inline unsigned long gh_pmd_index(unsigned long a) { int b = 9, s = 12 + 9; return (a >> s) & 511; }
static inline unsigned long gh_pte_index(unsigned long a) { return (a >> 12) & 511; }
static inline unsigned long gh_pxd_page_vaddr(u64 v) { return v & 0x0000FFFFFFFFF000UL; }

#define pr_info(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define pr_warn(...) do {} while (0)

/* PTE 位 */
#define PTE_VALID (1UL << 0)
#define PTE_TYPE_PAGE (3UL << 0)
#define PTE_USER (1UL << 6)
#define PTE_RDONLY (1UL << 7)
#define PTE_SHARED (3UL << 8)
#define PTE_AF (1UL << 10)
#define PTE_NG (1UL << 11)
#define PTE_UXN (1UL << 54)
#define PTE_ATTRINDX_NORMAL (0UL << 2)
#define PTE_TABLE_BIT (1UL << 1)
#define GH_PXD_TYPE_TABLE 0x3UL

#include <errno.h>
#define current (g_current_mm)
/* core harness 只测统计；PTE 拷贝在 pgtable harness 验证（pgtable.c 内有守卫） */
#include "ghostmem_pgtable.c"

/* ---- struct ghostmem_block（internal.h 被屏蔽，需补齐） ---- */
struct ghostmem_block {
    struct list_head list;
    void *mm;
    unsigned long va;
    unsigned long nr_pages;
    unsigned long *pfns;
};

/* ---- VMA 桩：模拟 find_vma 返回的 vm_area_struct ---- */
struct stub_vma { unsigned long vm_start, vm_end; struct stub_vma *next; };
static struct stub_vma *g_vmas = NULL;

void *stub_find_vma(void *mm, unsigned long addr)
{
    (void)mm;
    /* find_vma 语义：返回第一个 vm_start <= addr < vm_end；否则第一个 vm_start > addr；否则 NULL */
    struct stub_vma *best = NULL;
    for (struct stub_vma *v = g_vmas; v; v = v->next) {
        if (v->vm_start <= addr && addr < v->vm_end) return v;
        if (v->vm_start > addr && (!best || v->vm_start < best->vm_start)) best = v;
    }
    return best;
}
/* ghostmem.c 的 kfunc 指针变量：此处提供定义（非宏） */
void *(*kfunc_find_vma)(void *mm, unsigned long addr) = stub_find_vma;
/* ghostmem.c 内 spin_lock 宏来自 internal.h（被屏蔽）：此处补宏 */
#define spin_lock(l) gh_raw_spin_lock(&(l)->rlock)
#define spin_unlock(l) gh_raw_spin_unlock(&(l)->rlock)

/* ghostmem.c 提供 block_list/lock/in_flight（include 方式） */
#include "ghostmem.c"

/* ---- 测试 ---- */
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

static void add_vma(unsigned long start, unsigned long end)
{
    struct stub_vma *v = calloc(1, sizeof(*v));
    v->vm_start = start; v->vm_end = end; v->next = g_vmas; g_vmas = v;
}

int main(void)
{
    stub_mm mm;
    u64 pgd[512];
    memset(pgd, 0, sizeof(pgd));
    mm.pgd = pgd;

    /* 模拟：heap [0x10000000, 0x10004000) 与一个库 [0x30000000, 0x30008000) */
    add_vma(0x10000000, 0x10004000);
    add_vma(0x30000000, 0x30008000);

    /* 1. gh_find_hole：应落在 4GB 起点（第一个空洞） */
    {
        unsigned long hole = gh_find_hole(&mm, 0x1000);
        CHECK(hole == 0x100000000UL, "hole starts at scan base 4GB");
    }

    /* 2. 块大小超空洞则跳过（模拟 5GB 空洞在 0x140000000..0x140001000 被占） */
    add_vma(0x140000000UL, 0x140001000UL);
    {
        unsigned long hole = gh_find_hole(&mm, 0x2000);
        CHECK(hole == 0x100000000UL, "hole before 5GB block");
        /* 大块请求（4GB）：4GB..5GB gap 仅 1GB 不够，跳过 5GB 块 → 落在块尾 */
        unsigned long hole2 = gh_find_hole(&mm, 0x100000000UL);
        CHECK(hole2 == 0x140001000UL, "large hole skips 5GB block to its end");
    }

    /* 3. do_alloc prot==0 归一化为 RWX */
    {
        unsigned long va;
        int ret = ghostmem_do_alloc(&mm, 2, 0, &va);
        CHECK(ret == 0, "do_alloc(prot=0) ok");
        CHECK(va != 0, "alloc returns VA");
        u64 *ptep = ghostmem_get_pte(&mm, va);
        CHECK(ptep && (*ptep & PTE_USER), "alloc PTE user");
        CHECK(ptep && !(*ptep & PTE_RDONLY), "alloc PTE writable (RWX default)");
        CHECK(ptep && !(*ptep & PTE_UXN), "alloc PTE executable (RWX default)");
        CHECK(ghostmem_do_free(&mm, va) == 0, "do_free ok");
        ptep = ghostmem_get_pte(&mm, va);
        CHECK(ptep == NULL || !(*ptep & PTE_VALID), "PTE cleared after free");
    }

    /* 4. do_alloc 越界拒绝 */
    {
        unsigned long va;
        CHECK(ghostmem_do_alloc(&mm, 0, 0, &va) == -EINVAL, "alloc 0 pages rejected");
        CHECK(ghostmem_do_alloc(&mm, GHOSTMEM_MAX_PAGES + 1, 0, &va) == -EINVAL,
              "alloc over max rejected");
    }

    /* 5. do_free 未登记地址 */
    CHECK(ghostmem_do_free(&mm, 0xdead0000UL) == -EINVAL, "free unknown VA rejected");

    /* 6. 释放全部块（模块卸载路径） */
    {
        unsigned long va;
        ghostmem_do_alloc(&mm, 1, 0, &va);
        ghostmem_free_blocks_for_mm(&mm, "test");
        CHECK(ghostmem_do_free(&mm, va) == -EINVAL, "block freed via free_blocks_for_mm");
    }

    /* 7. do_info：统计目标 mm 的块数/页数（任意 pid 语义，缓冲解耦） */
    {
        unsigned long va;
        struct ghostmem_stats st = { 0, 0 };
        ghostmem_do_alloc(&mm, 3, 0, &va);
        CHECK(ghostmem_do_info(&mm, (void *)&st, sizeof(st)) == 0, "do_info ok");
        CHECK(st.nr_blocks == 1 && st.nr_pages == 3, "do_info counts 1 block / 3 pages");
        /* 释放后归零 */
        ghostmem_do_free(&mm, va);
        memset(&st, 0, sizeof(st));
        CHECK(ghostmem_do_info(&mm, (void *)&st, sizeof(st)) == 0, "do_info after free");
        CHECK(st.nr_blocks == 0 && st.nr_pages == 0, "do_info zero after free");
        /* 越界缓冲拒绝 */
        CHECK(ghostmem_do_info(&mm, (void *)&st, 4) == -EINVAL, "info len too small rejected");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
