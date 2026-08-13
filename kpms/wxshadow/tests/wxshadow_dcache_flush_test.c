/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow dcache flush 行遍历逻辑执行级测试。
 *
 * 验证 wxshadow_flush_kern_dcache_area 的核心：
 *   - CTR_EL0.DminLine 编码 → cache 行大小（4 << dminline）
 *   - 对齐起始、步进覆盖、范围计算
 * 逻辑抽取自 wxshadow_internal.h（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_dc kpms/wxshadow/tests/wxshadow_dcache_flush_test.c
 *   /tmp/wx_dc
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned long long u64;

/* 行大小提取（CTR_EL0.DminLine 位 [19:16]） */
static unsigned long dmin_line_size(u64 ctr_el0)
{
    return 4UL << ((ctr_el0 >> 16) & 0xf);
}

/* flush 范围计算：返回需处理的起始地址与结束（模拟遍历计数） */
static void compute_flush_range(unsigned long kva, unsigned long size,
                                unsigned long line_size,
                                unsigned long *out_start, unsigned long *out_end)
{
    *out_start = kva & ~(line_size - 1);
    *out_end = kva + size;
}

/* 模拟 flush：统计 dc cvau 次数 */
static unsigned long simulate_flush(unsigned long kva, unsigned long size,
                                    unsigned long line_size)
{
    unsigned long addr, end, count = 0;
    end = kva + size;
    for (addr = kva & ~(line_size - 1); addr < end; addr += line_size)
        count++;
    return count;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. CTR_EL0 编码 → 行大小 */
    CHECK(dmin_line_size(0x0) == 4, "DminLine=0 -> 4 bytes");
    CHECK(dmin_line_size(0x4UL << 16) == 64, "DminLine=4 -> 64 bytes");
    CHECK(dmin_line_size(0x6UL << 16) == 256, "DminLine=6 -> 256 bytes");

    /* 2. 对齐起始 + 范围 */
    {
        unsigned long start, end;
        compute_flush_range(0x1000 + 16, 64, 64, &start, &end);
        CHECK(start == 0x1000, "aligned to line start");
        CHECK(end == 0x1050, "end = kva+size"); /* 0x1000+16+64 */
    }

    /* 3. 遍历计数（覆盖行数） */
    {
        /* 64 字节行，flush 129 字节（从 0x1020 起）→ 行 0x1000,0x1040,0x1080 = 3 行 */
        unsigned long n = simulate_flush(0x1020, 129, 64);
        CHECK(n == 3, "129 bytes @64-line -> 3 lines");
        /* 精确页内：0x1000 起 64 字节 → 1 行 */
        CHECK(simulate_flush(0x1000, 64, 64) == 1, "64 bytes aligned -> 1 line");
        /* 4 字节行 flush 100 字节 → 25 行 */
        CHECK(simulate_flush(0x1000, 100, 4) == 25, "4-byte line -> 25 lines");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
