/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow_client 行为测试（host 可执行路径）。
 * 无内核模块时 prctl 操作应干净失败（退出 1），-h 用法正常。
 *
 * 编译：gcc -o /tmp/wx_client_e2e tests/wxshadow_client_e2e_test.c
 * 运行：/tmp/wx_client_e2e <wxshadow_client二进制>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

static int run(const char *bin, char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        execv(bin, argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char *argv[])
{
    const char *bin = argc > 1 ? argv[1] : "./wxshadow_client";
    char *a[8];
    int rc;

    /* 1. -h 用法退出 0 */
    a[0] = (char *)bin; a[1] = (char *)"-h"; a[2] = NULL;
    rc = run(bin, a);
    CHECK(rc == 0, "-h exits 0");

    /* 2. 缺 -p → usage 退出 1 */
    a[0] = (char *)bin; a[1] = (char *)"-a"; a[2] = (char *)"0x1000"; a[3] = NULL;
    rc = run(bin, a);
    CHECK(rc == 1, "missing -p exits 1");

    /* 3. -t 合法模式无模块失败干净（退出 1） */
    a[0] = (char *)bin; a[1] = (char *)"-p"; a[2] = (char *)"0";
    a[3] = (char *)"-t"; a[4] = (char *)"1"; a[5] = NULL;
    rc = run(bin, a);
    CHECK(rc == 1, "-t 1 without module exits 1");

    /* 4. -t 非法模式（越界）同样失败 */
    a[3] = (char *)"-t"; a[4] = (char *)"99"; a[5] = NULL;
    rc = run(bin, a);
    CHECK(rc == 1, "-t 99 (out of range) exits 1");

    /* 5. 断点设置无模块失败干净 */
    a[1] = (char *)"-p"; a[2] = (char *)"1";
    a[3] = (char *)"-a"; a[4] = (char *)"0x1000"; a[5] = NULL;
    rc = run(bin, a);
    CHECK(rc == 1, "-a without module exits 1");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
