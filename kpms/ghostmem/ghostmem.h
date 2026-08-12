/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem KPM Module - VMA-Less Ghost Memory
 * Copyright (C) 2024
 *
 * 内核侧幽灵内存：分配物理页 + 手动建立 PTE 页表项，不登记任何 VMA，
 * 使该内存在 /proc/<pid>/maps 中不可见。
 */

#ifndef _KPM_GHOSTMEM_H_
#define _KPM_GHOSTMEM_H_

#include <ktypes.h>
#include <stdbool.h>

/* Page size constants (4K pages) */
#define GHOSTMEM_PAGE_SHIFT     12
#define GHOSTMEM_PAGE_SIZE      (1UL << GHOSTMEM_PAGE_SHIFT)
#define GHOSTMEM_PAGE_MASK      (~(GHOSTMEM_PAGE_SIZE - 1))

/* prctl options for ghostmem ("GM" prefix = 0x47474d) */
#define PR_GHOSTMEM_ALLOC       0x47474d01  /* prctl(opt, pid, nr_pages, prot, 0) -> user VA */
#define PR_GHOSTMEM_FREE        0x47474d02  /* prctl(opt, pid, va, 0, 0) */
#define PR_GHOSTMEM_INFO        0x47474d03  /* prctl(opt, pid, buf, len, 0) -> stats to user buf */

/* Max pages per allocation (64 = 256KB) */
#define GHOSTMEM_MAX_PAGES      64

/*
 * Ghost memory permission bits (mirror PROT_* semantics):
 * 1 = read, 2 = write, 4 = exec.  prot == 0 means RWX.
 */
#define GHOSTMEM_PROT_READ      0x1
#define GHOSTMEM_PROT_WRITE     0x2
#define GHOSTMEM_PROT_EXEC      0x4

/* Stats reported by PR_GHOSTMEM_INFO */
struct ghostmem_stats {
    u64 nr_blocks;      /* Number of live ghost blocks for this mm */
    u64 nr_pages;       /* Total ghost pages for this mm */
};

#endif /* _KPM_GHOSTMEM_H_ */
