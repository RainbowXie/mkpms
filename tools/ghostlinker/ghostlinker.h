/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostLinker - 在幽灵内存中手工映射 ELF 的自定义 Linker v1
 *
 * 目标（对齐视频方案 #3）：不依赖系统 linker / soinfo，在 VMA-Less
 * 幽灵内存中完成 ET_DYN 的手工映射、段装载、RELA 重定位与 init_array 调用，
 * 使加固壳的 soinfo / linker 符号比对找不到我们。
 *
 * v1 范围（见 docs/ghostlinker.md）：
 *   - 仅 ELF64 ET_DYN（PIE/共享库）
 *   - RELA 重定位：R_AARCH64_ABS64 / GLOB_DAT / JUMP_SLOT / RELATIVE
 *   - 外部符号通过宿主回调解析（默认解析失败即报错，不做猜测）
 *   - 非目标：TLS、IFUNC、异常展开表、init_array 参数语义
 *
 * 内存来源：
 *   1. 优先 prctl(PR_GHOSTMEM_ALLOC) —— 幽灵内存（模块已加载时）
 *   2. 失败回退 mmap(MAP_ANONYMOUS|MAP_PRIVATE) —— 仅本机测试用
 *
 * Copyright (C) 2024
 */

#ifndef _GHOSTLINKER_H_
#define _GHOSTLINKER_H_

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 符号解析回调：返回 0 表示成功并把地址写入 *out_addr */
typedef int (*gh_sym_resolver_fn)(const char *name, void **out_addr, void *udata);

/* 内存分配回调：返回分配基址；NULL 时用默认（ghostmem prctl → mmap 回退） */
typedef void *(*gh_alloc_fn)(size_t size, void *udata);

struct gh_linker_cb {
    gh_sym_resolver_fn resolve;
    void *udata;
    gh_alloc_fn alloc;              /* 可选 */
    void *alloc_udata;              /* 可选 */
};

struct gh_linker_result {
    void *base;                     /* 映射基址（幽灵内存 VA 或 mmap 回退） */
    size_t size;                    /* 总映射大小 */
    int nr_init;                    /* init_array 项数 */
    void **init_array;              /* init_array 指针（base 内，非副本） */
    const Elf64_Sym *symtab;        /* 内部状态，供 gh_link_find_symbol 使用 */
    const char *strtab;
    unsigned long load_base;        /* PT_LOAD 最小 vaddr，符号换算用 */
    int sym_count;                  /* 动态符号表条目数 */
};

/*
 * 在幽灵内存中加载 ELF。
 * elf/elf_size: ELF64 文件内容；cb: 回调（resolve 可为 NULL 表示无外部符号）。
 * 成功返回 0，失败返回负 errno。
 */
int gh_link_elf(const void *elf, size_t elf_size, const struct gh_linker_cb *cb,
                struct gh_linker_result *out);

/* 释放由 gh_link_elf 分配的映射（ghostmem prctl 或 munmap 回退） */
void gh_link_free(struct gh_linker_result *res);

/* 在已加载模块中按名查找导出符号（主动调用 / 测试用）；未找到返回 NULL */
void *gh_link_find_symbol(const struct gh_linker_result *res, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _GHOSTLINKER_H_ */
