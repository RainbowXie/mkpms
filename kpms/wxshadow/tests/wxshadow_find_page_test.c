/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow page 链表查找（wxshadow_find_page）执行级测试。
 *
 * 验证：mm + 页对齐地址匹配、命中 refcount++、未命中 NULL、锁配对。
 * 逻辑抽取自 wxshadow.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_fp kpms/wxshadow/tests/wxshadow_find_page_test.c
 *   /tmp/wx_fp
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

typedef unsigned long long u64;
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* ---- list_head 桩 ---- */
struct list_head { struct list_head *next, *prev; };
static void init_list(struct list_head *l) { l->next = l; l->prev = l; }
static void add_tail(struct list_head *n, struct list_head *h)
{
    n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
#define list_for_each(pos, h) for (pos = (h)->next; pos != (h); pos = pos->next)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))

/* ---- 锁桩 ---- */
static int lock_count = 0, unlock_count = 0;
static void spin_lock_stub(void *l) { (void)l; lock_count++; }
static void spin_unlock_stub(void *l) { (void)l; unlock_count++; }
#define spin_lock spin_lock_stub
#define spin_unlock spin_unlock_stub
static void *global_lock;

/* wxshadow_page 桩 */
struct wxshadow_page_stub {
    struct list_head list;
    void *mm;
    unsigned long page_addr;
    int refcount;
};

/* 同步自 wxshadow.c */
static struct wxshadow_page_stub *wxshadow_find_page(
    struct list_head *page_list, void *mm, unsigned long addr)
{
    struct list_head *pos;
    struct wxshadow_page_stub *page;
    unsigned long target_addr = addr & PAGE_MASK;

    spin_lock(&global_lock);
    list_for_each(pos, page_list) {
        page = container_of(pos, struct wxshadow_page_stub, list);
        if (page->mm == mm && page->page_addr == target_addr) {
            page->refcount++;
            spin_unlock(&global_lock);
            return page;
        }
    }
    spin_unlock(&global_lock);
    return NULL;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    struct list_head page_list;
    struct wxshadow_page_stub p1, p2;
    void *mm1 = (void *)0x111, *mm2 = (void *)0x222;

    init_list(&page_list);
    p1 = (struct wxshadow_page_stub){ .mm = mm1, .page_addr = 0x1000, .refcount = 1 };
    p2 = (struct wxshadow_page_stub){ .mm = mm2, .page_addr = 0x2000, .refcount = 1 };
    add_tail(&p1.list, &page_list);
    add_tail(&p2.list, &page_list);

    /* 1. 精确命中（mm + addr 页对齐） */
    lock_count = unlock_count = 0;
    CHECK(wxshadow_find_page(&page_list, mm1, 0x1000) == &p1, "find p1 by mm+addr");
    CHECK(p1.refcount == 2, "refcount incremented on hit");
    CHECK(lock_count == 1 && unlock_count == 1, "lock paired on hit");

    /* 2. 页对齐：addr 在页内任意偏移命中同页 */
    lock_count = unlock_count = 0;
    CHECK(wxshadow_find_page(&page_list, mm2, 0x2abc) == &p2, "find p2 by page-aligned addr");
    CHECK(p2.refcount == 2, "p2 refcount incremented");

    /* 3. mm 不匹配 → NULL（地址对但 mm 错） */
    CHECK(wxshadow_find_page(&page_list, mm1, 0x2000) == NULL, "mm mismatch -> NULL");

    /* 4. addr 不匹配 → NULL */
    CHECK(wxshadow_find_page(&page_list, mm1, 0x3000) == NULL, "addr mismatch -> NULL");

    /* 5. 空链表 → NULL */
    {
        struct list_head empty;
        init_list(&empty);
        CHECK(wxshadow_find_page(&empty, mm1, 0x1000) == NULL, "empty list -> NULL");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
