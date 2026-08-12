/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ghostmem_client 区间重叠逻辑 host 单测。
 * 编译：gcc -Itests/shim -o /tmp/gc_test tests/ghostmem_client_overlap_test.c \
 *          kpms/ghostmem/ghostmem_client.c
 * 但 client 有 main，改为 -DGHOSTMEM_CLIENT_UNIT_TEST 编译测试模式。
 */
#define GHOSTMEM_CLIENT_UNIT_TEST
#include "ghostmem_client.c"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    /* 区间 [100, 100+16) 与各种 VMA 的相交判定 */
    CHECK(ranges_overlap(100, 16, 100, 200) == 1, "start equal");
    CHECK(ranges_overlap(100, 16, 115, 200) == 1, "probe inside vma");
    CHECK(ranges_overlap(100, 16, 50, 110) == 1, "vma overlaps probe start");
    CHECK(ranges_overlap(100, 16, 50, 100) == 0, "vma ends at probe start");
    CHECK(ranges_overlap(100, 16, 116, 200) == 0, "vma starts after probe end");
    CHECK(ranges_overlap(100, 16, 116, 200) == 0, "adjacent, no overlap");
    CHECK(ranges_overlap(0x1000, 0x1000, 0x2000, 0x3000) == 0, "page-aligned adjacent");
    CHECK(ranges_overlap(0x1000, 0x1000, 0x1000, 0x2000) == 1, "page-aligned shared start");

    /* parse_prot：--prot 字符串 → 位掩码 */
    CHECK(parse_prot("rwx") == (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC),
          "parse rwx");
    CHECK(parse_prot("r--") == GHOSTMEM_PROT_READ, "parse r");
    CHECK(parse_prot("rx") == (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_EXEC), "parse rx");
    CHECK(parse_prot("w") == GHOSTMEM_PROT_WRITE, "parse w");
    /* 空/未知：默认 RWX */
    CHECK(parse_prot("") == (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC),
          "empty -> RWX default");
    CHECK(parse_prot("zzz") == (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC),
          "unknown -> RWX default");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
