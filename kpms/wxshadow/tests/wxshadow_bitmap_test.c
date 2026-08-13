/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow 脏位图范围置位执行级测试。
 *
 * 验证 wxshadow_bitmap_set_range：
 *   - 范围内逐位置位
 *   - 页界裁剪（offset+len > PAGE_SIZE）
 *   - 边界（NULL/越界/len=0）
 * 逻辑抽取自 wxshadow.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_bm kpms/wxshadow/tests/wxshadow_bitmap_test.c
 *   /tmp/wx_bm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define WXSHADOW_DIRTY_WORD_BITS (sizeof(unsigned long) * 8)
#define WXSHADOW_DIRTY_BITMAP_WORDS \
    ((PAGE_SIZE + WXSHADOW_DIRTY_WORD_BITS - 1) / WXSHADOW_DIRTY_WORD_BITS)

/* 同步自 wxshadow.c */
static void wxshadow_bitmap_set_range(unsigned long *bitmap,
                                      unsigned long offset,
                                      unsigned long len)
{
    unsigned long i;

    if (!bitmap || offset >= PAGE_SIZE || len == 0)
        return;

    if (offset + len > PAGE_SIZE)
        len = PAGE_SIZE - offset;

    for (i = offset; i < offset + len; i++) {
        unsigned long word = i / WXSHADOW_DIRTY_WORD_BITS;
        unsigned long bit = i % WXSHADOW_DIRTY_WORD_BITS;
        bitmap[word] |= (1UL << bit);
    }
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

static int is_set(const unsigned long *bm, unsigned long off)
{
    return (bm[off / WXSHADOW_DIRTY_WORD_BITS] >> (off % WXSHADOW_DIRTY_WORD_BITS)) & 1;
}

int main(void)
{
    unsigned long bm[WXSHADOW_DIRTY_BITMAP_WORDS];

    /* 1. 范围置位 */
    memset(bm, 0, sizeof(bm));
    wxshadow_bitmap_set_range(bm, 10, 5);
    CHECK(is_set(bm, 10) && is_set(bm, 14) && !is_set(bm, 9) && !is_set(bm, 15),
          "range 10..14 set, neighbors clear");

    /* 2. 跨 word 边界（64 位 word：置位 60..70） */
    memset(bm, 0, sizeof(bm));
    wxshadow_bitmap_set_range(bm, 60, 11);
    CHECK(is_set(bm, 60) && is_set(bm, 70) && !is_set(bm, 59) && !is_set(bm, 71),
          "cross-word range set");

    /* 3. 页界裁剪：offset+len 超页 → 只置位到页尾 */
    memset(bm, 0, sizeof(bm));
    wxshadow_bitmap_set_range(bm, PAGE_SIZE - 3, 10);
    CHECK(is_set(bm, PAGE_SIZE - 3) && is_set(bm, PAGE_SIZE - 1) &&
          !is_set(bm, PAGE_SIZE), "clipped at page end");

    /* 4. 边界：offset 越界 / len=0 / NULL */
    memset(bm, 0, sizeof(bm));
    wxshadow_bitmap_set_range(bm, PAGE_SIZE, 5);
    CHECK(!is_set(bm, 0), "offset >= PAGE_SIZE ignored");
    wxshadow_bitmap_set_range(bm, 0, 0);
    CHECK(!is_set(bm, 0), "len 0 ignored");
    wxshadow_bitmap_set_range(NULL, 0, 5); /* 不崩 */
    CHECK(1, "NULL bitmap no-op");

    /* 5. 整页置位 */
    memset(bm, 0, sizeof(bm));
    wxshadow_bitmap_set_range(bm, 0, PAGE_SIZE);
    CHECK(is_set(bm, 0) && is_set(bm, 4095), "full page set");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
