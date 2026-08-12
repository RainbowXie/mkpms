/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ghostmem_client 行为测试：host 上可真实执行的路径。
 * 覆盖 -c（maps 校验，读 /proc/self/maps）+ 参数解析错误路径。
 * prctl 相关操作（alloc/free）在无内核模块时预期失败，仅验证流程不崩。
 *
 * 编译：gcc -Ikpms/ghostmem -o /tmp/ghostmem_client_e2e \
 *          tests/ghostmem_client_e2e_test.c
 * 运行：/tmp/ghostmem_client_e2e <ghostmem_client二进制>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

/* 运行 client 子进程，返回退出码 */
static int run_client(const char *bin, char *const argv[])
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
    const char *bin = argc > 1 ? argv[1] : "./ghostmem_client";
    char mypid[32];
    char va0[32];
    char *self_argv[8];
    int rc;

    snprintf(mypid, sizeof(mypid), "%d", (int)getpid());

    /* 1. 无 pid → usage 退出码 1 */
    self_argv[0] = (char *)bin; self_argv[1] = (char *)"-a"; self_argv[2] = (char *)"16";
    self_argv[3] = NULL;
    rc = run_client(bin, self_argv);
    CHECK(rc == 1, "missing -p returns 1");

    /* 2. 非法选项 → usage */
    self_argv[1] = (char *)"-Z"; self_argv[2] = NULL;
    rc = run_client(bin, self_argv);
    CHECK(rc == 1, "unknown option returns 1");

    /* 3. -c 校验本进程 maps 中某地址不可见（返回 0，除非恰好重叠） */
    snprintf(va0, sizeof(va0), "0x%lx", 0x7f0000000000UL);
    self_argv[0] = (char *)bin; self_argv[1] = (char *)"-p"; self_argv[2] = mypid;
    self_argv[3] = (char *)"-c"; self_argv[4] = va0; self_argv[5] = NULL;
    rc = run_client(bin, self_argv);
    CHECK(rc == 0, "-c on unmapped VA exits 0 (invisible)");

    /* 4. -i（pid=0）在无模块时预期失败但流程不崩（退出 1） */
    self_argv[1] = (char *)"-p"; self_argv[2] = mypid;
    self_argv[3] = (char *)"-i"; self_argv[4] = NULL;
    rc = run_client(bin, self_argv);
    CHECK(rc == 1, "-i without module fails cleanly");

    /* 5. -a 无模块失败干净（不崩溃） */
    self_argv[1] = (char *)"-p"; self_argv[2] = mypid;
    self_argv[3] = (char *)"-a"; self_argv[4] = (char *)"16"; self_argv[5] = NULL;
    rc = run_client(bin, self_argv);
    CHECK(rc == 1, "-a without module fails cleanly");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
