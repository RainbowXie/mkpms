/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow VMA 偏移检测逻辑执行级测试。
 *
 * 验证 scan_vma_struct_offsets 的 vm_mm 搜索核心：在 vma 内存的
 * [0x10..0x80] 区间寻找指向 mm 的指针值。检测逻辑纯逻辑可测：
 *   - vm_mm 在预期偏移 → 找到并返回正确偏移
 *   - vm_mm 不在区间 → 回退默认 0x40
 * 逻辑抽取自 wxshadow_scan.c（副本同步）。
 *
 * 编译运行：
 *   gcc -o /tmp/wx_vma_test kpms/wxshadow/tests/wxshadow_vma_offset_test.c
 *   /tmp/wx_vma_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef unsigned long long u64;

/* 模拟 vm_area_struct 内存（vm_start@0x00, vm_end@0x08, 其余任意） */
struct vma_mem {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned char pad[0x70];
};
#define safe_read_u64(addr, out) (*(out) = *(volatile u64 *)(addr), 1)

/* 检测核心：搜索 [0x10..0x80] 中 == mm 的指针 */
static int scan_vm_mm(void *vma, void *mm, int *out_offset)
{
    int i;
    for (i = 0x10; i < 0x80; i += 8) {
        u64 val;
        if (!safe_read_u64((unsigned long)vma + i, &val))
            continue;
        if (val == (u64)mm) {
            *out_offset = i;
            return 1;
        }
    }
    return 0;
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(void)
{
    struct vma_mem vma;
    void *mm = (void *)0xdead0000UL;
    int off = -1;

    /* 1. vm_mm 在偏移 0x40（真实内核常见布局） */
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = 0x1000; vma.vm_end = 0x4000;
    *(void **)((char *)&vma + 0x40) = mm;
    CHECK(scan_vm_mm(&vma, mm, &off) == 1, "vm_mm found");
    CHECK(off == 0x40, "vm_mm offset = 0x40");
    CHECK(vma.vm_start == 0x1000 && vma.vm_end == 0x4000, "vm_start/end intact");

    /* 2. vm_mm 在偏移 0x28（其他内核版本布局） */
    memset(&vma, 0, sizeof(vma));
    *(void **)((char *)&vma + 0x28) = mm;
    CHECK(scan_vm_mm(&vma, mm, &off) == 1 && off == 0x28, "vm_mm at 0x28 found");

    /* 3. vm_mm 不在区间 → 未找到（回退默认 0x40 由调用方处理） */
    memset(&vma, 0, sizeof(vma));
    *(void **)((char *)&vma + 0x90) = mm; /* 超出搜索区间 */
    CHECK(scan_vm_mm(&vma, mm, &off) == 0, "vm_mm outside range not found");

    /* 4. 搜索不受 vm_start/vm_end 值干扰（非 mm 指针的字段） */
    memset(&vma, 0, sizeof(vma));
    vma.vm_start = (unsigned long)mm; /* 看似指针但不在搜索区间起点前 */
    *(void **)((char *)&vma + 0x50) = mm;
    CHECK(scan_vm_mm(&vma, mm, &off) == 1 && off == 0x50, "search skips start/end fields");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
