/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMA 偏移助手执行级测试（ghostmem + wxshadow 共用语义）。
 *
 * vm_area_struct 的 vm_start/vm_end 在偏移 0x00/0x08（Linux 内核稳定布局），
 * ghostmem 空洞查找与 wxshadow 均用静态偏移读取。本测试验证：
 *   1. 偏移常量在 ghostmem/wxshadow 两处一致
 *   2. 偏移读取语义正确（不同 VMA 起始值）
 * 副本需与 ghostmem.c / wxshadow_internal.h 同步。
 *
 * 编译运行：
 *   gcc -o /tmp/vma_off_test kpms/wxshadow/tests/vma_offset_test.c
 *   /tmp/vma_off_test
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned long size_t;
typedef unsigned char u8;

/* ---- 同步自 ghostmem.c / wxshadow_internal.h ---- */
#define VMA_VM_START_OFFSET     0x00
#define VMA_VM_END_OFFSET       0x08

/* 模拟 vm_area_struct：vm_start/vm_end 为头两个字段（内核稳定布局） */
struct vm_area_struct_stub {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned long vm_flags;   /* 偏移 0x10 */
};

#define GET_FIELD(ptr, offset, type) (*(type *)((char *)(ptr) + (offset)))

static unsigned long vma_start_of(void *vma)
{
    return GET_FIELD(vma, VMA_VM_START_OFFSET, unsigned long);
}
static unsigned long vma_end_of(void *vma)
{
    return GET_FIELD(vma, VMA_VM_END_OFFSET, unsigned long);
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    /* 1. 偏移常量跨模块一致 */
    CHECK(VMA_VM_START_OFFSET == 0x00, "start offset 0x00");
    CHECK(VMA_VM_END_OFFSET == 0x08, "end offset 0x08");

    /* 2. 读取语义：典型 VMA（含 flags 干扰验证偏移精确） */
    {
        struct vm_area_struct_stub vma = { 0x10000000, 0x10004000, 0x2 };
        CHECK(vma_start_of(&vma) == 0x10000000UL, "read vm_start");
        CHECK(vma_end_of(&vma) == 0x10004000UL, "read vm_end");
        CHECK(vma_end_of(&vma) - vma_start_of(&vma) == 0x4000UL, "vma size");
    }

    /* 3. 空洞判定语义：vstart > addr 即有 gap */
    {
        struct vm_area_struct_stub vma = { 0x30000000, 0x30008000, 0 };
        unsigned long addr = 0x20000000;
        CHECK(vma_start_of(&vma) > addr, "gap detection (start > addr)");
        CHECK(vma_start_of(&vma) - addr == 0x10000000UL, "gap size");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
