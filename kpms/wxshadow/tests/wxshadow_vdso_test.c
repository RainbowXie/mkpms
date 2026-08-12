/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow vdso 检测用户指针筛选逻辑执行级测试。
 *
 * scan_by_vdso_elf_magic 在 mm 内存中扫描 vdso 指针，先筛选用户指针：
 *   - val != 0
 *   - (val >> 48) == 0（用户地址，非内核 KVA）
 * 然后验证指向 ELF 魔数。本测试锁定筛选 + ELF 魔数判定逻辑。
 * 逻辑抽取自 wxshadow_scan.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_vdso_test kpms/wxshadow/tests/wxshadow_vdso_test.c
 *   /tmp/wx_vdso_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned long long u64;

/* 筛选：候选是否可能是用户指针（非 0 且高 48 位为 0） */
static int is_user_ptr_candidate(u64 val)
{
    if (val == 0)
        return 0;
    if ((val >> 48) != 0)
        return 0;
    return 1;
}

/* ELF 魔数检查（用户地址 → 模拟读取） */
static int check_elf_magic_simple(unsigned char *mem_at_uaddr)
{
    return mem_at_uaddr[0] == 0x7f && mem_at_uaddr[1] == 'E' &&
           mem_at_uaddr[2] == 'L' && mem_at_uaddr[3] == 'F';
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. 用户指针筛选 */
    CHECK(is_user_ptr_candidate(0x7f0000000000UL), "user ptr accepted");
    CHECK(is_user_ptr_candidate(0x1000UL), "low user ptr accepted");
    CHECK(!is_user_ptr_candidate(0UL), "NULL rejected");
    CHECK(!is_user_ptr_candidate(0xffff800000000000UL), "kernel KVA rejected");
    CHECK(!is_user_ptr_candidate(0x0001ffff00000000UL), "high user (48+ bit) rejected");

    /* 2. ELF 魔数判定 */
    {
        unsigned char elf[4] = { 0x7f, 'E', 'L', 'F' };
        unsigned char not_elf[4] = { 0x7f, 'E', 'L', 'X' };
        CHECK(check_elf_magic_simple(elf), "ELF magic matched");
        CHECK(!check_elf_magic_simple(not_elf), "non-ELF not matched");
    }

    /* 3. 组合：模拟 scan_by_vdso 的完整判定 */
    {
        /* mm 内存：0x100..0x3F8 区间的指针槽 */
        unsigned char mm_mem[0x400];
        unsigned char vdso_mem[4] = { 0x7f, 'E', 'L', 'F' };
        unsigned long pgd_off = 0x80; /* 模拟 mm->pgd 偏移 */
        unsigned long vdso_user_addr = 0x70000000UL;
        int found_off = -1;
        int offset;

        memset(mm_mem, 0, sizeof(mm_mem));
        /* 在 mm+0x200 槽放 vdso 用户指针 */
        *(u64 *)(mm_mem + 0x200) = vdso_user_addr;

        for (offset = pgd_off + 0x100; offset < pgd_off + 0x400; offset += 8) {
            u64 val = *(u64 *)(mm_mem + offset);
            if (!is_user_ptr_candidate(val))
                continue;
            /* 模拟：uaddr 指向 vdso_mem（ELF 魔数） */
            if (val == vdso_user_addr && check_elf_magic_simple(vdso_mem)) {
                found_off = offset;
                break;
            }
        }
        CHECK(found_off == 0x200, "vdso found at mm+0x200");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
