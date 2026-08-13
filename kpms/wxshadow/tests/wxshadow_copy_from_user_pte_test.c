/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow copy_from_user_via_pte 页边界检查执行级测试。
 *
 * PATCH 接口经 PTE 读用户缓冲，约束为单页内（buf_off + len <= PAGE_SIZE）：
 * 跨页缓冲被拒绝。本测试锁定该边界语义。
 * 逻辑抽取自 wxshadow_bp.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_cfup_test kpms/wxshadow/tests/wxshadow_copy_from_user_pte_test.c
 *   /tmp/wx_cfup_test
 */
#include <stdio.h>
#include <stdint.h>

#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* 页边界检查：返回 0 = 允许（单页内），1 = 拒绝（跨页） */
static int check_page_boundary(unsigned long uaddr, unsigned long len)
{
    unsigned long buf_off = uaddr & ~PAGE_MASK;
    if (buf_off + len > PAGE_SIZE)
        return 1; /* 跨页拒绝 */
    return 0;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. 页内任意偏移 + 短 len → 允许 */
    CHECK(check_page_boundary(0x1000UL, 8) == 0, "page start, short len ok");
    CHECK(check_page_boundary(0x1ff0UL, 16) == 0, "near page end, fits ok");
    CHECK(check_page_boundary(0x100000000UL, 1) == 0, "any page, 1 byte ok");

    /* 2. 恰好到页尾 → 允许（buf_off+len == PAGE_SIZE） */
    CHECK(check_page_boundary(0x1f00UL, 0x100) == 0, "exactly page end ok");

    /* 3. 跨页 → 拒绝 */
    CHECK(check_page_boundary(0x1ff0UL, 32) == 1, "cross page rejected");
    CHECK(check_page_boundary(0x2000UL, 8) == 0, "aligned next page ok");
    CHECK(check_page_boundary(0x100000000UL + 0xff0, 32) == 1, "cross page at high va");

    /* 4. len == PAGE_SIZE 对齐 → 允许（buf_off=0） */
    CHECK(check_page_boundary(0x2000UL, PAGE_SIZE) == 0, "full page ok");
    CHECK(check_page_boundary(0x2f00UL, PAGE_SIZE) == 1, "full page from offset rejected");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
