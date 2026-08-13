/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow task_struct 链表遍历（wx_next_task）执行级测试。
 *
 * 历史 bug（CLAUDE.md 记录）：旧实现 `next - task_struct_offset.tasks_offset`
 * 是指针算术（乘以 sizeof(list_head)=16），应为字节偏移。
 * 本测试锁定修复：非零 tasks_offset 下遍历正确。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_next_task_test kpms/wxshadow/tests/wxshadow_next_task_test.c
 *   /tmp/wx_next_task_test
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* ---- 桩：list_head 与 task_struct 布局 ---- */
struct list_head { struct list_head *next, *prev; };

/* 模拟 task_struct：tasks_offset 处嵌 list_head */
struct task_struct_stub {
    char head[0x100];          /* 前导字段（模拟 comm 等） */
    struct list_head tasks;    /* tasks 链表节点 */
    int id;                    /* 标识 */
};
#define TASKS_OFFSET offsetof(struct task_struct_stub, tasks)

/* ---- 同步自 wxshadow_internal.h ---- */
static struct task_struct_stub *wx_next_task(struct task_struct_stub *task)
{
    struct list_head *head = (struct list_head *)((char *)task + TASKS_OFFSET);
    struct list_head *next = head->next;
    return (struct task_struct_stub *)((char *)next - TASKS_OFFSET);
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    struct task_struct_stub tasks[4];
    int i;

    for (i = 0; i < 4; i++)
        tasks[i].id = i + 1;

    /* 建环：t0->t1->t2->t3->t0 */
    for (i = 0; i < 4; i++)
        tasks[i].tasks.next = &tasks[(i + 1) % 4].tasks;

    /* 非零 offset 下遍历：验证字节偏移算术（原 bug 触发条件） */
    CHECK(TASKS_OFFSET != 0, "tasks_offset non-zero (exercises byte-arithmetic)");

    {
        struct task_struct_stub *cur = &tasks[0];
        int visited[4] = {0};
        int steps = 0;
        /* 走 8 步（两圈），应循环访问 1,2,3,4 */
        while (steps < 8) {
            visited[cur->id - 1]++;
            cur = wx_next_task(cur);
            steps++;
        }
        CHECK(visited[0] == 2 && visited[1] == 2 &&
              visited[2] == 2 && visited[3] == 2,
              "circular traversal visits all 4 tasks evenly");
        CHECK(cur == &tasks[0], "traversal returns to start after 4 steps");
    }

    /* 单节点环 */
    {
        tasks[0].tasks.next = &tasks[0].tasks;
        struct task_struct_stub *cur = &tasks[0];
        int steps = 0;
        while (steps < 3) {
            cur = wx_next_task(cur);
            steps++;
        }
        CHECK(cur == &tasks[0], "single-node ring stays on itself");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
