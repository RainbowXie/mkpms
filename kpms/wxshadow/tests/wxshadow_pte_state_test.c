/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxshadow PTE 状态机逻辑执行级测试。
 *
 * 提取 wxshadow_pgtable.c 的四个状态构建函数（restore/hidden/stepping）
 * 与 make_pte/replace_pte_pfn，验证 W^X Shadow 状态机的 PTE 位语义：
 *   SHADOW_X(--x) ↔ ORIGINAL(r--) ↔ STEPPING(r-x)
 * 副本需与 wxshadow_pgtable.c 同步（源函数同名同逻辑）。
 *
 * 编译运行：
 *   gcc -Ikpms/wxshadow -Ikpms/ghostmem/tests/shim \
 *       -o /tmp/wx_pte_test kpms/wxshadow/tests/wxshadow_pte_state_test.c
 *   /tmp/wx_pte_test
 */
#include <stdio.h>
#include <stdint.h>

typedef unsigned char u8;
typedef unsigned long long u64;
#define PAGE_SHIFT 12UL

/* ---- 同步自 wxshadow_pgtable.c / wxshadow.h ---- */
#define PTE_VALID (1UL << 0)
#define PTE_TYPE_PAGE (3UL << 0)
#define PTE_USER (1UL << 6)
#define PTE_RDONLY (1UL << 7)
#define PTE_SHARED (3UL << 8)
#define PTE_AF (1UL << 10)
#define PTE_NG (1UL << 11)
#define PTE_UXN (1UL << 54)
#define PTE_ATTRINDX_NORMAL (0UL << 2)

static u64 make_pte(unsigned long pfn, u64 prot)
{
    return (pfn << PAGE_SHIFT) | prot | PTE_VALID | PTE_TYPE_PAGE |
           PTE_AF | PTE_SHARED | PTE_NG | PTE_ATTRINDX_NORMAL;
}

/* wxshadow_page 桩：仅含 PTE 状态函数需要的字段 */
struct wxshadow_page_stub {
    u64 pte_original;
    unsigned long pfn_original;
};

static u64 wxshadow_replace_pte_pfn(u64 pte_template, unsigned long pfn)
{
    u64 entry = pte_template;
    entry &= ~0x0000FFFFFFFFF000UL;
    entry |= (pfn << PAGE_SHIFT) & 0x0000FFFFFFFFF000UL;
    entry |= PTE_VALID | PTE_TYPE_PAGE;
    return entry;
}

static u64 wxshadow_get_original_pte_template(struct wxshadow_page_stub *page)
{
    if (page && page->pte_original)
        return page->pte_original;
    if (page && page->pfn_original)
        return make_pte(page->pfn_original, PTE_USER | PTE_RDONLY);
    return 0;
}

static u64 wxshadow_build_restore_original_pte(struct wxshadow_page_stub *page)
{
    return wxshadow_get_original_pte_template(page);
}

static u64 wxshadow_build_hidden_original_pte(struct wxshadow_page_stub *page)
{
    u64 entry = wxshadow_get_original_pte_template(page);
    if (!entry || !page || !page->pfn_original)
        return 0;
    entry = wxshadow_replace_pte_pfn(entry, page->pfn_original);
    entry |= PTE_USER | PTE_RDONLY | PTE_UXN;
    return entry;
}

static u64 wxshadow_build_stepping_original_pte(struct wxshadow_page_stub *page)
{
    u64 entry = wxshadow_get_original_pte_template(page);
    if (!entry || !page || !page->pfn_original)
        return 0;
    entry = wxshadow_replace_pte_pfn(entry, page->pfn_original);
    entry |= PTE_USER | PTE_RDONLY;
    entry &= ~PTE_UXN;
    return entry;
}

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    struct wxshadow_page_stub page = {
        .pte_original = 0,
        .pfn_original = 0x12345UL,
    };

    /* make_pte 基座：USER|RDONLY，pfn 正确 */
    {
        u64 pte = make_pte(0x12345UL, PTE_USER | PTE_RDONLY);
        CHECK((pte >> 12) == 0x12345UL, "make_pte pfn");
        CHECK(pte & PTE_USER, "make_pte user");
        CHECK(pte & PTE_RDONLY, "make_pte readonly");
    }

    /* RESTORE：原始页 r--（USER|RDONLY|UXN） */
    {
        u64 pte = wxshadow_build_restore_original_pte(&page);
        CHECK((pte >> 12) == 0x12345UL, "restore pfn = original");
        CHECK(pte & PTE_USER, "restore user");
        CHECK(pte & PTE_RDONLY, "restore readonly (r--)");
    }

    /* HIDDEN：r-- + UXN（读原始内容，执行触发 fault） */
    {
        u64 pte = wxshadow_build_hidden_original_pte(&page);
        CHECK(pte & PTE_RDONLY, "hidden readonly (r--)");
        CHECK(pte & PTE_UXN, "hidden non-exec (UXN)");
        CHECK(pte & PTE_USER, "hidden user");
    }

    /* STEPPING：r-x（读原始 + 执行原始指令） */
    {
        u64 pte = wxshadow_build_stepping_original_pte(&page);
        CHECK(pte & PTE_RDONLY, "stepping readonly (r--)");
        CHECK(!(pte & PTE_UXN), "stepping executable (no UXN)");
        CHECK(pte & PTE_USER, "stepping user");
    }

    /* 状态机对比：stepping 与 hidden 仅 UXN 位不同（r-x vs r--） */
    {
        u64 h = wxshadow_build_hidden_original_pte(&page);
        u64 s = wxshadow_build_stepping_original_pte(&page);
        CHECK((h & ~PTE_UXN) == (s & ~PTE_UXN), "hidden vs stepping differ only in UXN");
        CHECK((h & PTE_UXN) && !(s & PTE_UXN), "UXN: hidden=1 stepping=0");
    }

    /* replace_pte_pfn：仅换 pfn，保留权限位 */
    {
        u64 base = make_pte(0x111, PTE_USER | PTE_RDONLY | PTE_UXN);
        u64 rep = wxshadow_replace_pte_pfn(base, 0x222UL);
        /* pfn 位 = bits[51:12]；高位权限位（如 UXN bit54）不计入 */
        CHECK(((rep & 0x0000FFFFFFFFF000UL) >> 12) == 0x222UL, "replace pfn");
        CHECK((rep & (PTE_USER | PTE_RDONLY | PTE_UXN)) ==
              (base & (PTE_USER | PTE_RDONLY | PTE_UXN)), "replace keeps perms");
    }

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
