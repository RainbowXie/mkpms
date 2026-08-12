/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow task_struct comm 偏移搜索逻辑执行级测试。
 *
 * 验证 find_comm_offset 核心：在 [0x400..0x1800) 区间搜索 "swapper"/"swapper/0"
 * 字符串（带 null 终止或 "/0" 后缀验证）。
 * 逻辑抽取自 wxshadow_scan.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_comm_test kpms/wxshadow/tests/wxshadow_comm_offset_test.c
 *   /tmp/wx_comm_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define TASK_STRUCT_MAX_SIZE 0x1800
#define TASK_COMM_LEN 16

/* safe_read_str 桩：从内核内存复制（模拟拷贝） */
static int safe_read_str(unsigned long addr, char *buf, int len)
{
    memcpy(buf, (const void *)addr, len);
    return 1;
}

/* 同步自 wxshadow_scan.c */
static int find_comm_offset(void *task)
{
    int i;
    char buf[16];

    for (i = 0x400; i < TASK_STRUCT_MAX_SIZE; i += 4) {
        if (!safe_read_str((unsigned long)task + i, buf, sizeof(buf)))
            continue;
        if (buf[0] == 's' && buf[1] == 'w' && buf[2] == 'a' &&
            buf[3] == 'p' && buf[4] == 'p' && buf[5] == 'e' && buf[6] == 'r') {
            if (buf[7] == '\0' || (buf[7] == '/' && buf[8] == '0')) {
                return i;
            }
        }
    }
    return -1;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    unsigned char task[TASK_STRUCT_MAX_SIZE];

    /* 1. "swapper"（null 终止）在 0x800 找到 */
    memset(task, 0, sizeof(task));
    strcpy((char *)task + 0x800, "swapper");
    CHECK(find_comm_offset(task) == 0x800, "comm 'swapper' at 0x800");

    /* 2. "swapper/0" 在 0x400 找到（区间起点） */
    memset(task, 0, sizeof(task));
    strcpy((char *)task + 0x400, "swapper/0");
    CHECK(find_comm_offset(task) == 0x400, "comm 'swapper/0' at 0x400");

    /* 3. 4 字节步进扫描：偏移 0x404（对齐）能找到 */
    memset(task, 0, sizeof(task));
    strcpy((char *)task + 0x404, "swapper");
    CHECK(find_comm_offset(task) == 0x404, "comm at aligned 0x404");

    /* 4. 相似但不匹配（"swapperX" 无终止）不误报 */
    memset(task, 0, sizeof(task));
    memcpy(task + 0x800, "swapperX", 8); /* X 非 \0 或 /0 */
    CHECK(find_comm_offset(task) == -1, "'swapperX' not matched");

    /* 5. 空 task（无 comm）→ -1 */
    memset(task, 0, sizeof(task));
    CHECK(find_comm_offset(task) == -1, "no comm -> -1");

    /* 6. 真实内核 comm 常为 "swapper/0" 变体在任意 4 对齐偏移 */
    memset(task, 0, sizeof(task));
    strcpy((char *)task + 0x1234, "swapper/0");
    CHECK(find_comm_offset(task) == 0x1234, "comm at 0x1234");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
