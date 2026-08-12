/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem KPM Module - Symbol Resolution
 *
 * 从 vmlinux 解析所需内核符号（不遍历模块，避免潜在挂起）。
 * 与 wxshadow 的 lookup_name_safe 同方案。
 *
 * Copyright (C) 2024
 */

#include "ghostmem_internal.h"

struct gh_lookup_data {
    const char *name;
    unsigned long addr;
};

static int gh_lookup_callback(void *data, const char *name, struct module *mod,
                              unsigned long addr)
{
    struct gh_lookup_data *ld = data;

    if (strcmp(name, ld->name) == 0) {
        ld->addr = addr;
        return 1; /* stop iteration */
    }
    return 0;
}

unsigned long gh_lookup_name(const char *name)
{
    struct gh_lookup_data ld = { .name = name, .addr = 0 };

    if (kallsyms_on_each_symbol)
        kallsyms_on_each_symbol(gh_lookup_callback, &ld);
    return ld.addr;
}

#define GH_RESOLVE(name) \
    do { \
        kfunc_##name = (typeof(kfunc_##name))gh_lookup_name(#name); \
        if (!kfunc_##name) { \
            pr_err("ghostmem: failed to find symbol: %s\n", #name); \
            return -1; \
        } \
    } while (0)

int ghostmem_resolve_symbols(void)
{
    pr_info("ghostmem: resolving symbols...\n");

    GH_RESOLVE(find_vma);
    GH_RESOLVE(get_task_mm);
    GH_RESOLVE(mmput);
    GH_RESOLVE(__get_free_pages);
    GH_RESOLVE(free_pages);
    GH_RESOLVE(kzalloc);
    GH_RESOLVE(kfree);
    GH_RESOLVE(copy_from_kernel_nofault);

    /* rcu_read_lock 是 inline 宏，vmlinux 导出符号为 __rcu_read_lock（wxshadow 同款） */
    kfunc_rcu_read_lock = (void *)gh_lookup_name("__rcu_read_lock");
    kfunc_rcu_read_unlock = (void *)gh_lookup_name("__rcu_read_unlock");
    if (!kfunc_rcu_read_lock || !kfunc_rcu_read_unlock) {
        pr_err("ghostmem: rcu functions not found\n");
        return -1;
    }

    kfunc_exit_mmap = (void *)gh_lookup_name("exit_mmap");
    if (!kfunc_exit_mmap) {
        pr_err("ghostmem: exit_mmap not found, refusing to load\n");
        return -1;
    }

    kvar_memstart_addr = (s64 *)gh_lookup_name("memstart_addr");
    if (!kvar_memstart_addr) {
        pr_err("ghostmem: memstart_addr not found\n");
        return -1;
    }

    kvar_physvirt_offset = (s64 *)gh_lookup_name("physvirt_offset");
    if (!kvar_physvirt_offset)
        pr_warn("ghostmem: physvirt_offset not found, using memstart mode\n");

    gh_raw_spin_lock = (void *)gh_lookup_name("_raw_spin_lock");
    gh_raw_spin_unlock = (void *)gh_lookup_name("_raw_spin_unlock");
    kfunc_find_task_by_vpid = (void *)gh_lookup_name("find_task_by_vpid");
    if (!gh_raw_spin_lock || !gh_raw_spin_unlock || !kfunc_find_task_by_vpid) {
        pr_err("ghostmem: spinlock/task functions not found\n");
        return -1;
    }

    return 0;
}
