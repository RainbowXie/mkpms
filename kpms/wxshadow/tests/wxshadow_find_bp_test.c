/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow 页内断点查找（wxshadow_find_bp）执行级测试。
 *
 * bps 数组线性搜索：active && addr == pc。
 * 逻辑抽取自 wxshadow.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_fbp kpms/wxshadow/tests/wxshadow_find_bp_test.c
 *   /tmp/wx_fbp
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define WXSHADOW_MAX_BPS_PER_PAGE 16

/* wxshadow_bp 桩 */
struct wxshadow_bp_stub { unsigned long addr; int active; };
struct wxshadow_page_stub {
    struct wxshadow_bp_stub bps[WXSHADOW_MAX_BPS_PER_PAGE];
    int nr_bps;
};

/* 同步自 wxshadow.c */
static struct wxshadow_bp_stub *wxshadow_find_bp(
    struct wxshadow_page_stub *page_info, unsigned long pc)
{
    int i;
    for (i = 0; i < page_info->nr_bps; i++) {
        if (page_info->bps[i].active && page_info->bps[i].addr == pc)
            return &page_info->bps[i];
    }
    return NULL;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    struct wxshadow_page_stub pg;

    /* 建 3 个断点（2 激活 1 非激活） */
    memset(&pg, 0, sizeof(pg));
    pg.nr_bps = 3;
    pg.bps[0] = (struct wxshadow_bp_stub){ 0x1000, 1 };
    pg.bps[1] = (struct wxshadow_bp_stub){ 0x2000, 0 }; /* 非激活 */
    pg.bps[2] = (struct wxshadow_bp_stub){ 0x3000, 1 };

    /* 1. 命中激活断点 */
    CHECK(wxshadow_find_bp(&pg, 0x1000) == &pg.bps[0], "find active bp[0]");
    CHECK(wxshadow_find_bp(&pg, 0x3000) == &pg.bps[2], "find active bp[2]");

    /* 2. 非激活断点地址 → NULL */
    CHECK(wxshadow_find_bp(&pg, 0x2000) == NULL, "inactive bp not found");

    /* 3. 未知地址 → NULL */
    CHECK(wxshadow_find_bp(&pg, 0x4000) == NULL, "unknown addr -> NULL");

    /* 4. 空页 → NULL */
    {
        struct wxshadow_page_stub empty;
        memset(&empty, 0, sizeof(empty));
        CHECK(wxshadow_find_bp(&empty, 0x1000) == NULL, "empty page -> NULL");
    }

    /* 5. 返回的是页内槽（可直接修改） */
    {
        struct wxshadow_bp_stub *bp = wxshadow_find_bp(&pg, 0x1000);
        bp->active = 0; /* 模拟删除 */
        CHECK(wxshadow_find_bp(&pg, 0x1000) == NULL, "slot modification visible");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
