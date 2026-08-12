/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ghostmem_pgtable 逻辑宿主可执行测试（harness）。
 *
 * 用桩实现 kfunc 指针与 gh_* 助手，使 ghostmem_pgtable.c 的逻辑
 * （PTE 构建/解除/空表回收）可被真实执行验证——此前仅语法检查。
 *
 * 页表模拟：arm64 4 级（PGD/PUD/PMD/PTE 各 512 项）。
 * 关键桩语义：__get_free_pages 保证页对齐（host malloc 需 2x 对齐），
 * __GFP_ZERO 语义由 memset 模拟。
 *
 * 编译运行：
 *   gcc -DGHOSTMEM_PGTABLE_HARNESS -Ikpms/ghostmem -Ikpms/ghostmem/tests/shim \
 *       -o /tmp/pg_harness tests/ghostmem_pgtable_harness.c
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 桩：ghostmem_internal.h 需要的框架类型/函数 ---- */
#define _KPM_GHOSTMEM_INTERNAL_H_
#define __user
#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)

#include "ghostmem.h"
#include "ghostmem_args.h"

/* kfunc 桩 */
void *(*kfunc_find_vma)(void *mm, unsigned long addr);
void *(*kfunc_get_task_mm)(void *task);
void (*kfunc_mmput)(void *mm);
void *kfunc_exit_mmap;
unsigned long (*kfunc___get_free_pages)(unsigned int gfp_mask, unsigned int order);
void (*kfunc_free_pages)(unsigned long addr, unsigned int order);
void *(*kfunc_kzalloc)(size_t size, unsigned int flags);
void (*kfunc_kfree)(void *ptr);
long (*kfunc_copy_from_kernel_nofault)(void *dst, const void *src, size_t size);
s64 *kvar_memstart_addr;
s64 *kvar_physvirt_offset;
unsigned long page_offset_base;
int gh_page_shift = 12;
int gh_page_level = 4;

/* ---- 页对齐分配器桩（模拟 __get_free_pages 的页对齐 + __GFP_ZERO） ---- */
static void *g_raw[256];
static int g_raw_count = 0;
static unsigned long g_pages = 0;

static void stub_register(unsigned long aligned, void *raw)
{
    if (g_raw_count < 256)
        g_raw[g_raw_count++] = raw;
    (void)aligned;
}

static unsigned long stub_get_free_pages(unsigned int gfp, unsigned int order)
{
    (void)gfp;
    size_t sz = (PAGE_SIZE << order) + PAGE_SIZE;
    unsigned char *raw = malloc(sz);
    if (!raw)
        return 0;
    unsigned long aligned = ((unsigned long)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    memset((void *)aligned, 0, PAGE_SIZE << order);
    stub_register(aligned, raw);
    g_pages++;
    return aligned;
}

static void stub_free_pages(unsigned long addr, unsigned int order)
{
    (void)order;
    for (int i = 0; i < g_raw_count; i++) {
        if ((unsigned long)g_raw[i] <= addr &&
            (unsigned long)g_raw[i] + 2 * PAGE_SIZE > addr) {
            free(g_raw[i]);
            g_raw[i] = g_raw[--g_raw_count];
            g_pages--;
            return;
        }
    }
}

#define kfunc___get_free_pages stub_get_free_pages
#define kfunc_free_pages stub_free_pages
#define kfunc_kzalloc(n, f) calloc(1, n)
#define kfunc_kfree free
#define kfunc_get_task_mm stub_gtm
#define kfunc_mmput stub_mmput
#define kfunc_copy_from_kernel_nofault NULL
#define kvar_memstart_addr NULL
#define kvar_physvirt_offset NULL
#define page_offset_base 0
#define kfunc_exit_mmap NULL
static void *stub_gtm(void *t) { (void)t; return NULL; }
static void stub_mmput(void *m) { (void)m; }

/* gh_* 助手（harness：物理地址 == 指针值，单地址空间） */
#define gh_is_kva(a) ((a) != 0)
#define gh_safe_read_u64(a, o) (*(o) = *(volatile u64 *)(a), 1)
typedef struct { u64 *pgd; } stub_mm;
#define gh_mm_pgd(mm) (((stub_mm *)mm)->pgd)
static inline unsigned long gh_kaddr_to_phys(unsigned long va) { return va; }
static inline unsigned long gh_phys_to_virt(unsigned long pa) { return pa; }
static inline unsigned long gh_kaddr_to_pfn(unsigned long va) { return va >> 12; }
static inline void *gh_pfn_to_kaddr(unsigned long pfn) { return (void *)(pfn << 12); }
#define gh_lock() do {} while (0)
#define gh_unlock() do {} while (0)

/* 索引助手（与内核一致，4 级 4K） */
static inline unsigned long gh_pgd_index(unsigned long a) {
    int b = 12 - 3, s = 12 + (4 - 1) * b;
    return (a >> s) & ((1UL << b) - 1);
}
static inline unsigned long gh_pud_index(unsigned long a) {
    int b = 12 - 3, s = 12 + 2 * b;
    return (a >> s) & ((1UL << b) - 1);
}
static inline unsigned long gh_pmd_index(unsigned long a) {
    int b = 12 - 3, s = 12 + b;
    return (a >> s) & ((1UL << b) - 1);
}
static inline unsigned long gh_pte_index(unsigned long a) {
    return (a >> PAGE_SHIFT) & 511;
}
static inline unsigned long gh_pxd_page_vaddr(u64 v) {
    return v & 0x0000FFFFFFFFF000UL;
}

/* PTE 位（与内核一致） */
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

#define ghostmem_copy_to_user_via_pte gh_ctu_stub_unused
#include <errno.h>
#define current ((void *)0)
#include "ghostmem_pgtable.c"

/* ---- 测试 ---- */
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

static unsigned long page_count(void) { return g_pages; }

int main(void)
{
    stub_mm mm;
    u64 pgd[512];
    unsigned long pfns[4];
    unsigned long va = 0x100000000UL; /* 4GB，2MB 对齐 */
    u64 *ptep;
    int i;

    memset(pgd, 0, sizeof(pgd));
    mm.pgd = pgd;

    /* 数据页 */
    for (i = 0; i < 4; i++)
        pfns[i] = gh_kaddr_to_pfn(stub_get_free_pages(0, 0));
    g_pages = 0; /* 只统计表页 */

    /* 1. map 4 页 RWX（prot=7） */
    CHECK(ghostmem_map_pages(&mm, va, pfns, 4, 7) == 0, "map 4 pages RWX");

    /* 2. PTE 内容：pfn + 权限 */
    ptep = ghostmem_get_pte(&mm, va);
    CHECK(ptep != NULL && (*ptep & PTE_VALID), "PTE present");
    CHECK(ptep && ((*ptep >> 12) & 0xFFFFFFFFFUL) == pfns[0], "PTE pfn matches");
    CHECK(ptep && (*ptep & PTE_USER), "PTE user");
    CHECK(ptep && !(*ptep & PTE_RDONLY), "PTE writable (RWX)");
    CHECK(ptep && !(*ptep & PTE_UXN), "PTE executable (RWX)");

    /* 3. 中间表页已分配 */
    CHECK(page_count() >= 1, "intermediate table pages allocated");

    /* 4. 末页 pfn */
    ptep = ghostmem_get_pte(&mm, va + 3 * PAGE_SIZE);
    CHECK(ptep && ((*ptep >> 12) & 0xFFFFFFFFFUL) == pfns[3], "PTE[3] pfn matches");

    /* 5. unmap：PTE 清除 + 空表回收（页计数归零 = 无泄漏） */
    ghostmem_unmap_pages(&mm, va, 4);
    ptep = ghostmem_get_pte(&mm, va);
    CHECK(ptep == NULL || !(*ptep & PTE_VALID), "PTE cleared after unmap");
    CHECK(page_count() == 0, "table pages reclaimed (no leak)");

    /* 6. 再 map/unmap（可重复性） */
    CHECK(ghostmem_map_pages(&mm, va, pfns, 4, 7) == 0, "re-map ok");
    ghostmem_unmap_pages(&mm, va, 4);
    CHECK(page_count() == 0, "re-unmap reclaims (no leak)");

    /* 7. 只读映射权限 */
    CHECK(ghostmem_map_pages(&mm, va, pfns, 1, GHOSTMEM_PROT_READ) == 0, "map RO");
    ptep = ghostmem_get_pte(&mm, va);
    CHECK(ptep && (*ptep & PTE_RDONLY), "RO PTE has RDONLY");
    CHECK(ptep && (*ptep & PTE_UXN), "RO PTE has UXN (no exec)");
    ghostmem_unmap_pages(&mm, va, 1);

    /* 8. 双块共享 PUD 表：块 A 释放不误回收 B 的表页 */
    {
        unsigned long va2 = 0x10001000UL; /* 同 2MB 组，跨 PTE 表? 否——同组 */
        unsigned long pfns_b[2];
        pfns_b[0] = gh_kaddr_to_pfn(stub_get_free_pages(0, 0));
        pfns_b[1] = gh_kaddr_to_pfn(stub_get_free_pages(0, 0));
        CHECK(ghostmem_map_pages(&mm, va, pfns, 2, 7) == 0, "block A map");
        CHECK(ghostmem_map_pages(&mm, va2, pfns_b, 2, 7) == 0, "block B map");
        unsigned long before = page_count();
        ghostmem_unmap_pages(&mm, va, 2);
        CHECK(page_count() < before, "A unmap frees some tables");
        ghostmem_unmap_pages(&mm, va2, 2);
        {
            /* 找残留：检查各级表 */
            u64 *pgd_e = mm.pgd + gh_pgd_index(va);
            fprintf(stderr, "  [dbg] pgd entry: %lx\n", (unsigned long)*pgd_e);
            if (*pgd_e & 0x3UL) {
                u64 *pud_e = (u64 *)gh_pxd_page_vaddr(*pgd_e) + gh_pud_index(va);
                if (*pud_e & 0x3UL) {
                    u64 *pmd_e = (u64 *)gh_pxd_page_vaddr(*pud_e) + gh_pmd_index(va);
                    if (*pmd_e & 0x3UL) {
                        u64 *pte = (u64 *)gh_pxd_page_vaddr(*pmd_e);
                        int nz = 0;
                        for (int k = 0; k < 512; k++) if (pte[k]) nz++;
                        (void)nz;
                    }
                }
            }
        }
        /* 表结构干净 = pgd 槽清空（表页全回收）；数据页在真实路径由
         * block 释放，harness 手动 free 仅避免泄漏。 */
        u64 *pgd_e = mm.pgd + gh_pgd_index(va);
        CHECK(*pgd_e == 0, "B unmap reclaims all tables");
        for (int di = 0; di < 4; di++)
            stub_free_pages(pfns[di] << 12, 0);
        stub_free_pages(pfns_b[0] << 12, 0);
        stub_free_pages(pfns_b[1] << 12, 0);
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
