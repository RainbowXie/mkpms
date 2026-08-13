/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow task_struct tasks_offset 检测逻辑执行级测试。
 *
 * 验证 detect_task_struct_offsets 的 tasks_offset 搜索核心（4 重验证）：
 *   1. next/prev 都是内核 VA（is_kva）
 *   2. next != prev（非单节点）
 *   3. next->prev == self（双向链表一致）
 *   4. candidate 的 comm == "init"
 * 逻辑抽取自 wxshadow_scan.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_tasks_test kpms/wxshadow/tests/wxshadow_tasks_offset_test.c
 *   /tmp/wx_tasks_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define TASK_STRUCT_MAX_SIZE 0x1800
typedef unsigned long long u64;

/* ---- 桩 ---- */
static int is_kva(unsigned long addr)
{
    return (addr >> 48) == 0xffff;
}
/* KVA 基座：task_to_kva 加的真实偏移，读时减回 */
#define KVA_BASE 0xffff800000000000UL
static int safe_read_u64(unsigned long addr, u64 *out)
{
    *out = *(volatile u64 *)(addr - KVA_BASE);
    return 1;
}
static int safe_read_str(unsigned long addr, char *buf, int len)
{
    memcpy(buf, (const void *)(addr - KVA_BASE), len);
    return 1;
}

/* 模拟 task_struct 内存 */
struct task_mem {
    unsigned char bytes[TASK_STRUCT_MAX_SIZE];
};
static unsigned long task_to_kva(struct task_mem *t)
{
    return KVA_BASE + (unsigned long)t; /* 模拟内核 VA */
}

/* 同步自 wxshadow_scan.c：tasks_offset 检测核心（comm_offset 参数化） */
static int detect_tasks_offset(struct task_mem *task, int comm_offset,
                               int search_start, int search_end, int *out_off)
{
    int i;
    unsigned long task_kva = task_to_kva(task);

    for (i = search_start; i < search_end; i += 8) {
        unsigned long list_addr = task_kva + i;
        u64 next_va, prev_va;
        if (!safe_read_u64(list_addr, &next_va)) continue;
        if (!safe_read_u64(list_addr + 8, &prev_va)) continue;
        if (!is_kva(next_va) || !is_kva(prev_va)) continue;
        if (next_va == prev_va) continue;
        {   /* next->prev == self */
            u64 next_prev;
            if (!safe_read_u64(next_va + 8, &next_prev)) continue;
            if (next_prev != list_addr) continue;
        }
        {   /* candidate comm == "init" */
            void *candidate = (void *)(next_va - i);
            char comm_buf[8];
            if (!safe_read_str((unsigned long)candidate + comm_offset, comm_buf, sizeof(comm_buf))) continue;
            if (comm_buf[0]=='i' && comm_buf[1]=='n' && comm_buf[2]=='i' && comm_buf[3]=='t') {
                *out_off = i;
                return 1;
            }
        }
    }
    return 0;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    struct task_mem task;
    struct task_mem init_task;
    int off = -1;
    const int comm_off = 0x800;
    const int tasks_off = 0x400;

    memset(&task, 0, sizeof(task));
    memset(&init_task, 0, sizeof(init_task));
    strcpy((char *)init_task.bytes + comm_off, "init");

    /* 建立链表：task.tasks(tasks_off) 指向 init_task.tasks，且 next->prev==self */
    {
        unsigned long self_list = task_to_kva(&task) + tasks_off;
        unsigned long init_list = task_to_kva(&init_task) + tasks_off;
        /* task.tasks.next = &init_task.tasks（next != prev） */
        *(u64 *)(task.bytes + tasks_off) = init_list;
        *(u64 *)(task.bytes + tasks_off + 8) = 0xffff800000100000UL; /* prev 指向别处（≠next） */
        /* init.tasks.prev == self（next->prev 校验） */
        *(u64 *)(init_task.bytes + tasks_off) = init_list;      /* next 自环 */
        *(u64 *)(init_task.bytes + tasks_off + 8) = self_list;  /* prev == task.tasks */
    }

    /* 1. 正常检测：tasks_offset 在 0x400 找到 */
    CHECK(detect_tasks_offset(&task, comm_off, 0x100, 0x600, &off) == 1,
          "tasks_offset found");
    CHECK(off == tasks_off, "tasks_offset = 0x400");

    /* 2. 搜索区间不含 0x400 → 未找到 */
    off = -1;
    CHECK(detect_tasks_offset(&task, comm_off, 0x500, 0x600, &off) == 0,
          "outside search range not found");

    /* 3. comm 不匹配（"swp" 非 "init"）→ 未找到 */
    {
        struct task_mem bad;
        memcpy(&bad, &task, sizeof(bad));
        strcpy((char *)bad.bytes + 0x800, "swp"); /* 假 init 的 comm 改掉 */
        /* 让 candidate（init_task 的副本）comm 变 */
        struct task_mem bad_init;
        memcpy(&bad_init, &init_task, sizeof(bad_init));
        strcpy((char *)bad_init.bytes + comm_off, "swp");
        /* 重建指向 bad_init */
        unsigned long self_list = task_to_kva(&bad) + tasks_off;
        unsigned long bad_init_list = task_to_kva(&bad_init) + tasks_off;
        *(u64 *)(bad.bytes + tasks_off) = bad_init_list;
        *(u64 *)(bad.bytes + tasks_off + 8) = bad_init_list;
        *(u64 *)(bad_init.bytes + tasks_off + 8) = self_list;
        off = -1;
        CHECK(detect_tasks_offset(&bad, comm_off, 0x100, 0x600, &off) == 0,
              "comm mismatch -> not found");
    }

    /* 4. next->prev != self（链表不一致）→ 跳过 */
    {
        struct task_mem corrupt;
        memcpy(&corrupt, &task, sizeof(corrupt));
        struct task_mem corrupt_init;
        memcpy(&corrupt_init, &init_task, sizeof(corrupt_init));
        unsigned long self_list = task_to_kva(&corrupt) + tasks_off;
        unsigned long corrupt_init_list = task_to_kva(&corrupt_init) + tasks_off;
        *(u64 *)(corrupt.bytes + tasks_off) = corrupt_init_list;
        *(u64 *)(corrupt.bytes + tasks_off + 8) = corrupt_init_list;
        /* init.prev 指向错误地址（≠ self） */
        *(u64 *)(corrupt_init.bytes + tasks_off + 8) = corrupt_init_list;
        off = -1;
        CHECK(detect_tasks_offset(&corrupt, comm_off, 0x100, 0x600, &off) == 0,
              "inconsistent list -> not found");
    }

    /* 5. next/prev 非 KVA → 跳过 */
    {
        struct task_mem bad_kva;
        memset(&bad_kva, 0, sizeof(bad_kva));
        *(u64 *)(bad_kva.bytes + tasks_off) = 0x1000UL; /* 用户地址，非 KVA */
        *(u64 *)(bad_kva.bytes + tasks_off + 8) = 0x2000UL;
        off = -1;
        CHECK(detect_tasks_offset(&bad_kva, comm_off, 0x100, 0x600, &off) == 0,
              "non-kva pointers -> skipped");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
