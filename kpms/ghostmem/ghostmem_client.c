/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem Client - userspace tool for PR_GHOSTMEM_* prctl interface.
 *
 * 用法示例:
 *   ghostmem_client -p <pid> -a 16              # 分配 16 页幽灵内存
 *   ghostmem_client -p <pid> -a 16 --prot r-x   # 指定权限
 *   ghostmem_client -p <pid> -f 0x7f00000000    # 释放
 *   ghostmem_client -p <pid> -w 0x7f00000000 9090   # 写 2 字节
 *   ghostmem_client -p <pid> -r 0x7f00000000 8  # 读 8 字节
 *   ghostmem_client -p <pid> -i                # 统计信息
 *   ghostmem_client -p <pid> -c 0x7f00000000    # 校验 maps 中不可见
 *   ghostmem_client -p <pid> -c 0x7f00000000 65536  # 校验 64KB 区间
 *   ghostmem_client -p <pid> -n 16              # 分配 16 页并自校验不可见
 */

/* process_vm_readv/writev 声明需要 _GNU_SOURCE */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <unistd.h>

/* 与内核 ghostmem.h 保持一致 */
#define PR_GHOSTMEM_ALLOC  0x47474d01
#define PR_GHOSTMEM_FREE   0x47474d02
#define PR_GHOSTMEM_INFO   0x47474d03
#define PR_GHOSTMEM_WRITE  0x47474d04
#define PR_GHOSTMEM_READ   0x47474d05

#define GHOSTMEM_PROT_READ  0x1
#define GHOSTMEM_PROT_WRITE 0x2
#define GHOSTMEM_PROT_EXEC  0x4

struct ghostmem_stats {
    unsigned long long nr_blocks;
    unsigned long long nr_pages;
};

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -p <pid> <op>\n"
        "  -a <pages> [--prot rwx]   alloc N pages of ghost memory, print VA\n"
        "  -f <va>                   free block at VA\n"
        "  -w <va> <hexbytes>        write hex bytes to VA\n"
        "  -r <va> <len>             read len bytes from VA (hex dump)\n"
        "  -i                        print ghostmem stats (pid=0 only)\n"
        "  -c <va> [len]            verify range absent from maps (exit 1 if visible)\n"
        "  -n <pages>                alloc + self-verify invisibility (exit 1 if visible)\n",        prog);
}

static int parse_prot(const char *s)
{
    int prot = 0;
    if (strchr(s, 'r')) prot |= GHOSTMEM_PROT_READ;
    if (strchr(s, 'w')) prot |= GHOSTMEM_PROT_WRITE;
    if (strchr(s, 'x')) prot |= GHOSTMEM_PROT_EXEC;
    return prot ? prot : (GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC);
}

/* 区间相交判定：[va, va+len) 与 [start, end) 是否有重叠 */
static int ranges_overlap(unsigned long va, unsigned long len,
                          unsigned long start, unsigned long end)
{
    return va < end && va + len > start;
}

/* 读 /proc/<pid>/maps，判断区间 [va, va+len) 是否与任何 VMA 重叠 */
static int range_in_maps(pid_t pid, unsigned long va, unsigned long len)
{
    char path[64], line[512];
    FILE *fp;
    int overlap = 0;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
            if (ranges_overlap(va, len, start, end)) {
                overlap = 1;
                break;
            }
        }
    }
    fclose(fp);
    return overlap;
}

#ifndef GHOSTMEM_CLIENT_UNIT_TEST
int main(int argc, char *argv[])
{
    pid_t pid = 0;
    unsigned long va = 0, len = 0, nr_pages = 0;
    int prot = 0, i, bad = 0;
    const char *hex = NULL;
    int op = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            nr_pages = strtoul(argv[++i], NULL, 0);
            op = 'a';
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            va = strtoul(argv[++i], NULL, 0);
            op = 'f';
        } else if (!strcmp(argv[i], "-w") && i + 2 < argc) {
            va = strtoul(argv[++i], NULL, 0);
            hex = argv[++i];
            op = 'w';
        } else if (!strcmp(argv[i], "-r") && i + 2 < argc) {
            va = strtoul(argv[++i], NULL, 0);
            len = strtoul(argv[++i], NULL, 0);
            op = 'r';
        } else if (!strcmp(argv[i], "-i")) {
            op = 'i';
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            va = strtoul(argv[++i], NULL, 0);
            op = 'c';
            /* 可选第二参数：校验长度（默认一页） */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                len = strtoul(argv[++i], NULL, 0);
            }
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            /* 分配后自校验块区间 maps 不可见 */
            nr_pages = strtoul(argv[++i], NULL, 0);
            op = 'n';
        } else if (!strcmp(argv[i], "--prot") && i + 1 < argc) {
            prot = parse_prot(argv[++i]);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (pid <= 0 || !op) {
        print_usage(argv[0]);
        return 1;
    }

    switch (op) {
    case 'a': {
        long ret = prctl(PR_GHOSTMEM_ALLOC, pid, nr_pages, prot, 0);
        if (ret < 0) {
            fprintf(stderr, "ALLOC failed: %s (errno=%d)\n", strerror(errno), errno);
            return 1;
        }
        printf("0x%lx\n", ret);
        break;
    }
    case 'f': {
        long ret = prctl(PR_GHOSTMEM_FREE, pid, va, 0, 0);
        if (ret < 0) {
            fprintf(stderr, "FREE failed: %s (errno=%d)\n", strerror(errno), errno);
            return 1;
        }
        printf("freed 0x%lx\n", va);
        break;
    }
    case 'w': {
        unsigned char buf[256];
        int n = 0;
        const char *p = hex;
        /* hex 字符串 -> 字节 */
        while (*p && n < (int)sizeof(buf)) {
            unsigned int b;
            if (sscanf(p, "%2x", &b) != 1)
                break;
            buf[n++] = (unsigned char)b;
            p += 2;
        }
        if (n == 0) {
            fprintf(stderr, "no bytes parsed\n");
            return 1;
        }
        if (range_in_maps(pid, va, n) > 0) {
            fprintf(stderr, "warning: range overlaps /proc/%d/maps (not ghost?)\n", pid);
        }
        if (pid == 0) {
            /* 自进程：幽灵页在自身地址空间，直接写 */
            for (i = 0; i < n; i++) {
                *(volatile unsigned char *)(va + i) = buf[i];
            }
        } else {
            /* 跨进程：优先内核 prctl（PTE 读写，无 ptrace 依赖）；
             * 模块未加载时回退 process_vm_writev。 */
            long kr = prctl(PR_GHOSTMEM_WRITE, pid, va, (unsigned long)buf, (unsigned long)n);
            if (kr < 0) {
                struct iovec local = { buf, (size_t)n };
                struct iovec remote = { (void *)va, (size_t)n };
                ssize_t wr = process_vm_writev(pid, &local, 1, &remote, 1, 0);
                if (wr != n) {
                    fprintf(stderr, "write failed: prctl=%ld writev=%zd (errno=%d)\n",
                            kr, wr, errno);
                    return 1;
                }
            }
        }
        printf("wrote %d byte(s) to 0x%lx (pid=%d)\n", n, va, pid);
        break;
    }
    case 'r': {
        unsigned long j;
        if (range_in_maps(pid, va, len) > 0) {
            fprintf(stderr, "warning: range overlaps /proc/%d/maps (not ghost?)\n", pid);
        }
        printf("read 0x%lx (pid=%d):\n", va, pid);
        if (pid == 0) {
            /* 自进程：直接读 */
            for (j = 0; j < len; j++) {
                printf("%02x ", *(volatile unsigned char *)(va + j));
                if ((j & 15) == 15)
                    printf("\n");
            }
            printf("\n");
        } else {
            /* 跨进程：优先内核 prctl（PTE 读写），回退 process_vm_readv */
            unsigned char *rbuf = malloc(len ? len : 1);
            long kr = prctl(PR_GHOSTMEM_READ, pid, va, (unsigned long)rbuf, (unsigned long)len);
            if (kr < 0) {
                struct iovec local = { rbuf, len };
                struct iovec remote = { (void *)va, len };
                ssize_t rd = process_vm_readv(pid, &local, 1, &remote, 1, 0);
                if (rd != (ssize_t)len) {
                    fprintf(stderr, "read failed: prctl=%ld readv=%zd (errno=%d)\n",
                            kr, rd, errno);
                    free(rbuf);
                    return 1;
                }
            }
            for (j = 0; j < len; j++) {
                printf("%02x ", rbuf[j]);
                if ((j & 15) == 15)
                    printf("\n");
            }
            printf("\n");
            free(rbuf);
        }
        break;
    }
    case 'i': {
        struct ghostmem_stats st;
        /* 统计 pid 目标进程的幽灵块（缓冲在 client 自身，PTE 拷贝） */
        if (prctl(PR_GHOSTMEM_INFO, pid, (unsigned long)&st, sizeof(st), 0) < 0) {
            fprintf(stderr, "INFO failed: %s (errno=%d)\n", strerror(errno), errno);
            return 1;
        }
        printf("blocks=%llu pages=%llu\n", st.nr_blocks, st.nr_pages);
        break;
    }
    case 'c': {
        unsigned long check_len = len ? len : 4096;
        int ov = range_in_maps(pid, va, check_len);
        if (ov < 0) {
            fprintf(stderr, "cannot read /proc/%d/maps\n", pid);
            return 1;
        }
        printf(ov ? "VISIBLE in maps (bad)\n" : "INVISIBLE in maps (ok)\n");
        if (ov)
            bad = 1;
        break;
    }
    case 'n': {
        /* 分配 + 自校验：验证幽灵块整区间在 maps 中不可见 */
        long ret = prctl(PR_GHOSTMEM_ALLOC, pid, nr_pages, 0, 0);
        if (ret < 0) {
            fprintf(stderr, "ALLOC failed: %s (errno=%d)\n", strerror(errno), errno);
            return 1;
        }
        {
            unsigned long block_len = nr_pages * 4096;
            int ov = range_in_maps(pid, ret, block_len);
            printf("alloc 0x%lx (%lu pages): %s\n", ret, nr_pages,
                   ov ? "VISIBLE (bad)" : "INVISIBLE (ok)");
            if (ov)
                bad = 1;
            /* 校验完释放 */
            prctl(PR_GHOSTMEM_FREE, pid, ret, 0, 0);
        }
        break;
    }
    default:
        break;
    }
    return bad ? 1 : 0;
}
#endif /* GHOSTMEM_CLIENT_UNIT_TEST */
