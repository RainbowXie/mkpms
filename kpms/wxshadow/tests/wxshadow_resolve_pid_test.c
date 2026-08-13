/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow resolve_pid_to_mm（pid→mm 解析）执行级测试。
 *
 * 分支逻辑：
 *   - pid==0 → current 的 mm
 *   - pid!=0 → RCU 保护下 find_task_by_vpid + get_task_mm
 *   - task 不存在 → NULL
 *   - 存在但无 mm → NULL
 * 逻辑抽取自 wxshadow_bp.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_pidmm kpms/wxshadow/tests/wxshadow_resolve_pid_test.c
 *   /tmp/wx_pidmm
 */
#include <stdio.h>
#include <stdint.h>

typedef int pid_t;

/* ---- 桩 ---- */
static int rcu_lock_count = 0, rcu_unlock_count = 0;
static void *current_mm = (void *)0x111;
static int find_result = 1; /* 1=找到 task, 0=不存在 */
static void *task_mm = (void *)0x222;
static void *stub_find_task(pid_t p) { (void)p; return find_result ? (void *)0x777 : NULL; }
static void *stub_get_task_mm(void *t) { return t == current_mm ? current_mm : (t ? task_mm : NULL); }
static void stub_rcu_lock(void) { rcu_lock_count++; }
static void stub_rcu_unlock(void) { rcu_unlock_count++; }
#define kfunc_get_task_mm stub_get_task_mm
#define kfunc_rcu_read_lock stub_rcu_lock
#define kfunc_rcu_unlock stub_rcu_unlock

/* 同步自 wxshadow_bp.c */
static void *resolve_pid_to_mm(pid_t pid)
{
    void *mm;
    if (pid == 0)
        return kfunc_get_task_mm(current_mm);
    kfunc_rcu_read_lock();
    {
        void *task = stub_find_task(pid);
        if (!task) {
            kfunc_rcu_unlock();
            return NULL;
        }
        mm = kfunc_get_task_mm(task);
    }
    kfunc_rcu_unlock();
    return mm;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    /* 1. pid==0 → current mm（无 RCU） */
    rcu_lock_count = rcu_unlock_count = 0;
    CHECK(resolve_pid_to_mm(0) == current_mm, "pid 0 -> current mm");
    CHECK(rcu_lock_count == 0 && rcu_unlock_count == 0, "no RCU for pid 0");

    /* 2. pid!=0 + task 存在 → task mm（RCU 配对） */
    rcu_lock_count = rcu_unlock_count = 0;
    find_result = 1; task_mm = (void *)0x222;
    CHECK(resolve_pid_to_mm(1234) == task_mm, "pid -> task mm");
    CHECK(rcu_lock_count == 1 && rcu_unlock_count == 1, "RCU lock/unlock paired");

    /* 3. task 不存在 → NULL（RCU 配对） */
    rcu_lock_count = rcu_unlock_count = 0;
    find_result = 0;
    CHECK(resolve_pid_to_mm(9999) == NULL, "unknown pid -> NULL");
    CHECK(rcu_lock_count == 1 && rcu_unlock_count == 1, "RCU paired on not-found");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
