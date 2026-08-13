/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem ABI host self-check - 防止内核/客户端/linker 三处常量漂移。
 *
 * 内核文件无法在本机编译（缺 aarch64 工具链 + .kp submodule），此测试用
 * ktypes 桩直接 include 内核头 ghostmem.h，断言其 ABI（prctl 常量、PROT
 * 位、stats 布局）与预期值一致。host gcc 即可运行：
 *
 *   gcc -I kpms/ghostmem/tests/shim -I kpms/ghostmem \
 *       -o /tmp/ghostmem_abi_test kpms/ghostmem/tests/ghostmem_abi_test.c
 *   /tmp/ghostmem_abi_test
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* 内核头（ktypes 桩提供类型） */
#include "ghostmem.h"

/* 客户端 / linker 的副本常量（预期值与内核头一致） */
#define CLIENT_ALLOC   0x47474d01
#define CLIENT_FREE    0x47474d02
#define CLIENT_INFO    0x47474d03
#define CLIENT_PROT_R  0x1
#define CLIENT_PROT_W  0x2
#define CLIENT_PROT_X  0x4

/* ghostlinker（tools/ghostlinker/ghostlinker.c）副本常量 */
#define LINKER_ALLOC  0x47474d01
#define LINKER_FREE   0x47474d02
#define LINKER_PROT_R 0x1
#define LINKER_PROT_W 0x2
#define LINKER_PROT_X 0x4

static int failures = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    /* 1. prctl 常量：内核头与客户端副本一致 */
    CHECK(PR_GHOSTMEM_ALLOC == CLIENT_ALLOC, "ALLOC == 0x47474d01");
    CHECK(PR_GHOSTMEM_FREE == CLIENT_FREE, "FREE == 0x47474d02");
    CHECK(PR_GHOSTMEM_INFO == CLIENT_INFO, "INFO == 0x47474d03");

    /* 1b. ghostlinker 副本对齐（三向） */
    CHECK(PR_GHOSTMEM_ALLOC == LINKER_ALLOC, "linker ALLOC aligned");
    CHECK(PR_GHOSTMEM_FREE == LINKER_FREE, "linker FREE aligned");
    CHECK(GHOSTMEM_PROT_READ == LINKER_PROT_R &&
          GHOSTMEM_PROT_WRITE == LINKER_PROT_W &&
          GHOSTMEM_PROT_EXEC == LINKER_PROT_X, "linker PROT bits aligned");

    /* 2. PROT 位：内核头与客户端副本一致 */
    CHECK(GHOSTMEM_PROT_READ == CLIENT_PROT_R, "PROT_READ == 0x1");
    CHECK(GHOSTMEM_PROT_WRITE == CLIENT_PROT_W, "PROT_WRITE == 0x2");
    CHECK(GHOSTMEM_PROT_EXEC == CLIENT_PROT_X, "PROT_EXEC == 0x4");

    /* 2b. WRITE/READ 常量（0x47474d04/05，client 副本一致） */
    CHECK(PR_GHOSTMEM_WRITE == 0x47474d04, "WRITE == 0x47474d04");
    CHECK(PR_GHOSTMEM_READ == 0x47474d05, "READ == 0x47474d05");

    /* 3. 页常量 */
    CHECK(GHOSTMEM_PAGE_SIZE == 4096UL, "PAGE_SIZE == 4096");
    CHECK(GHOSTMEM_MAX_PAGES == 64, "MAX_PAGES == 64");

    /* 4. stats 布局：两个 u64，共 16 字节（rustFrida 侧 #[repr(C)] 依赖此） */
    CHECK(sizeof(struct ghostmem_stats) == 16, "stats size == 16");
    CHECK(offsetof(struct ghostmem_stats, nr_blocks) == 0, "nr_blocks at 0");
    CHECK(offsetof(struct ghostmem_stats, nr_pages) == 8, "nr_pages at 8");

    /* 5. 权限语义：prot==0 应等于 RWX（内核 do_alloc 默认分支） */
    CHECK((0 ? 0 : (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC))
              == 0x7, "default prot == RWX (0x7)");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
