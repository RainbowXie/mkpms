/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow mm_get_asid（ASID 提取）执行级测试。
 *
 * ASID = mm->context.id 低 16 位（TLB 精确刷新的前提）：
 *   - 未检测偏移（<0）或 mm NULL → 0
 *   - 正常提取低 16 位
 * 逻辑抽取自 wxshadow_pgtable.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_asid kpms/wxshadow/tests/wxshadow_asid_test.c
 *   /tmp/wx_asid
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef unsigned long long u64;
typedef short int16_t;

/* mm 桩：context.id 在指定偏移 */
struct mm_stub { unsigned char pad[0x100]; u64 context_id; };
#define MM_CONTEXT_ID_OFF offsetof(struct mm_stub, context_id)

static int16_t mm_context_id_offset = MM_CONTEXT_ID_OFF;

/* 同步自 wxshadow_pgtable.c */
static u64 mm_get_asid(void *mm)
{
    u64 context_id;
    if (!mm || mm_context_id_offset < 0)
        return 0;
    context_id = *(u64 *)((char *)mm + mm_context_id_offset);
    return context_id & 0xFFFF;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    struct mm_stub mm;
    memset(&mm, 0, sizeof(mm));

    /* 1. 正常 ASID 提取（低 16 位） */
    mm.context_id = 0x1234;
    CHECK(mm_get_asid(&mm) == 0x1234, "asid = context.id low 16");

    /* 2. 高 16 位被掩掉 */
    mm.context_id = 0xDEAD1234;
    CHECK(mm_get_asid(&mm) == 0x1234, "high bits masked");

    /* 3. ASID 0（内核线程/无地址空间） */
    mm.context_id = 0;
    CHECK(mm_get_asid(&mm) == 0, "asid 0 when context 0");

    /* 4. 未检测偏移 → 0 */
    mm_context_id_offset = -1;
    mm.context_id = 0x5678;
    CHECK(mm_get_asid(&mm) == 0, "undetected offset -> 0");

    /* 5. mm NULL → 0 */
    mm_context_id_offset = MM_CONTEXT_ID_OFF;
    CHECK(mm_get_asid(NULL) == 0, "null mm -> 0");

    /* 6. 高位干扰（其他 context 字段） */
    mm.context_id = 0xFFFFFFFFFFFF1234ULL;
    CHECK(mm_get_asid(&mm) == 0x1234, "asid stable with high garbage");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
