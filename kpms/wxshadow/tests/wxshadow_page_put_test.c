/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow page 引用计数生命周期执行级测试。
 *
 * 验证 wxshadow_page_put：
 *   - refcount 递减，>0 时不释放
 *   - 归零时才释放（shadow 页 + patch 数据 + 结构体）
 *   - 锁内摘资源、锁外释放
 * 逻辑抽取自 wxshadow.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_pp kpms/wxshadow/tests/wxshadow_page_put_test.c
 *   /tmp/wx_pp
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned long long u64;
#define WXSHADOW_MAX_PATCHES_PER_PAGE 32

/* ---- 桩 ---- */
static int lock_count = 0, unlock_count = 0;
static int free_pages_calls = 0, kfree_calls = 0;
static void *freed_shadow = NULL;
static void *freed_page = NULL;
static void *global_lock;
static void spin_lock_stub(void *l) { (void)l; lock_count++; }
static void spin_unlock_stub(void *l) { (void)l; unlock_count++; }
static void kfree_stub(void *p) { kfree_calls++; if (p == freed_shadow) freed_shadow = NULL; }
#define spin_lock spin_lock_stub
#define spin_unlock spin_unlock_stub

/* wxshadow_page 桩（仅生命周期字段） */
struct wxshadow_page_stub {
    int refcount;
    void *shadow_page;
    int nr_patches;
    struct wxshadow_patch_stub { void *data; } patches[32];
};
#define kfunc_kfree kfree_stub

static void free_pages_stub(unsigned long addr, unsigned int order)
{
    (void)order;
    free_pages_calls++;
    freed_shadow = (void *)addr;
}

/* 同步自 wxshadow.c */
static void wxshadow_page_put(struct wxshadow_page_stub *page)
{
    int should_free;
    unsigned long shadow_vaddr = 0;
    void *patch_data[32];
    int nr_patch_data = 0;
    int i;

    spin_lock(&global_lock);
    should_free = (--page->refcount == 0);
    if (should_free) {
        if (page->shadow_page) {
            shadow_vaddr = (unsigned long)page->shadow_page;
            page->shadow_page = NULL;
        }
        for (i = 0; i < page->nr_patches; i++) {
            if (!page->patches[i].data) continue;
            patch_data[nr_patch_data++] = page->patches[i].data;
            page->patches[i].data = NULL;
        }
    }
    spin_unlock(&global_lock);

    if (should_free) {
        for (i = 0; i < nr_patch_data; i++)
            kfunc_kfree(patch_data[i]);
        if (shadow_vaddr) {
            free_pages_stub(shadow_vaddr, 0);
        }
        kfunc_kfree(page);
    }
}



static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. refcount 2→1：不释放 */
    {
        struct wxshadow_page_stub pg = { 2, (void *)0x1000, 0, {{0}} };
        lock_count = unlock_count = 0; free_pages_calls = 0; kfree_calls = 0;
        wxshadow_page_put(&pg);
        CHECK(pg.refcount == 1, "refcount decremented to 1");
        CHECK(free_pages_calls == 0 && kfree_calls == 0, "not freed above zero");
        CHECK(lock_count == 1 && unlock_count == 1, "lock/unlock paired");
    }

    /* 2. refcount 1→0：释放 shadow 页 + 结构 */
    {
        struct wxshadow_page_stub pg = { 1, (void *)0x1000, 0, {{0}} };
        free_pages_calls = 0; kfree_calls = 0;
        freed_shadow = NULL; freed_page = &pg;
        wxshadow_page_put(&pg);
        CHECK(pg.refcount == 0, "refcount zero");
        CHECK(free_pages_calls == 1, "shadow page freed");
        CHECK(kfree_calls >= 1, "struct freed");
        CHECK(pg.shadow_page == NULL, "shadow_page cleared before free");
    }

    /* 3. 带 patch 数据：归零时全部释放 */
    {
        struct wxshadow_page_stub pg = { 1, (void *)0x2000, 2, {{(void *)0xaa},{(void *)0xbb},{0}} };
        kfree_calls = 0; free_pages_calls = 0;
        wxshadow_page_put(&pg);
        CHECK(free_pages_calls == 1, "shadow freed with patches");
        CHECK(kfree_calls == 3, "2 patches + struct freed");
        CHECK(pg.patches[0].data == NULL && pg.patches[1].data == NULL,
              "patch data cleared");
    }

    /* 4. 无 shadow 页：仅结构释放 */
    {
        struct wxshadow_page_stub pg = { 1, NULL, 0, {{0}} };
        free_pages_calls = 0; kfree_calls = 0;
        wxshadow_page_put(&pg);
        CHECK(free_pages_calls == 0, "no shadow -> no free_pages");
        CHECK(kfree_calls == 1, "struct freed");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
