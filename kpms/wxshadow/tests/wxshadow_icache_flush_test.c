/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow icache flush 回退逻辑执行级测试。
 *
 * 验证 flush_icache_range 的路径选择：
 *   - kfunc___flush_icache_range 可用 → 用它
 *   - 否则 → 全局 icache 失效（dcache 已 clean 前提）
 * 逻辑抽取自 wxshadow_internal.h（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_ic kpms/wxshadow/tests/wxshadow_icache_flush_test.c
 *   /tmp/wx_ic
 */
#include <stdio.h>
#include <stdint.h>

/* 桩：可用性开关 + 计数器 */
static int flush_range_available = 1;
static int flush_range_calls = 0;
static int fallback_calls = 0;
static void fake_flush_icache_range(unsigned long s, unsigned long e)
{
    (void)s; (void)e;
    flush_range_calls++;
}
#define kfunc___flush_icache_range (flush_range_available ? fake_flush_icache_range : 0)

/* 同步自 wxshadow_internal.h（ic ialluis 用计数桩替代） */
static void flush_icache_range(unsigned long start, unsigned long end)
{
    if (kfunc___flush_icache_range) {
        kfunc___flush_icache_range(start, end);
        return; /* isb 在真实实现中，此处省略 */
    }
    fallback_calls++;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. 可用 → 用内核函数 */
    flush_range_available = 1;
    flush_range_calls = 0; fallback_calls = 0;
    flush_icache_range(0x1000, 0x2000);
    CHECK(flush_range_calls == 1, "kernel flush used when available");
    CHECK(fallback_calls == 0, "fallback not used");

    /* 2. 不可用 → 全局失效回退 */
    flush_range_available = 0;
    flush_range_calls = 0; fallback_calls = 0;
    flush_icache_range(0x1000, 0x2000);
    CHECK(fallback_calls == 1, "global icache invalidation fallback");
    CHECK(flush_range_calls == 0, "kernel flush not used");

    /* 3. 恢复 */
    flush_range_available = 1;
    flush_icache_range(0x3000, 0x4000);
    CHECK(flush_range_calls == 1, "recovered uses kernel flush");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
