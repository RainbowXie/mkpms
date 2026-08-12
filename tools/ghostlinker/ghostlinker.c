/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostLinker - ELF loader for ghost memory (implementation)
 *
 * 手工 ELF64 ET_DYN 加载器：段映射到幽灵内存（VMA-Less），处理 RELA 重定位
 * （RELATIVE / GLOB_DAT / JUMP_SLOT / ABS64），符号解析走宿主回调，
 * 最后调用 init_array。v1 不处理 TLS / IFUNC。
 *
 * Copyright (C) 2024
 */

#include "ghostlinker.h"

#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

/* 与内核 ghostmem.h 对齐 */
#define PR_GHOSTMEM_ALLOC  0x47474d01
#define PR_GHOSTMEM_FREE   0x47474d02

#define GHOSTMEM_PROT_READ  0x1
#define GHOSTMEM_PROT_WRITE 0x2
#define GHOSTMEM_PROT_EXEC  0x4

#define PAGE_SIZE_ 4096UL
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE_ - 1) & ~(PAGE_SIZE_ - 1))

/*
 * 默认内存来源：先尝试幽灵内存（内核模块已加载时），失败回退 mmap。
 * prctl 语义：prctl(opt, pid=0, nr_pages, prot, 0) -> VA。
 */
static void *default_alloc(size_t size, void *udata)
{
    (void)udata;
    unsigned long nr_pages = PAGE_ALIGN(size) / PAGE_SIZE_;
    long va = prctl(PR_GHOSTMEM_ALLOC, 0, nr_pages,
                    GHOSTMEM_PROT_READ | GHOSTMEM_PROT_WRITE | GHOSTMEM_PROT_EXEC, 0);

    if (va > 0)
        return (void *)va;

    /* 幽灵内存不可用（未加载模块）：本机测试回退 */
    return mmap(NULL, PAGE_ALIGN(size), PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void default_free(void *base, size_t size)
{
    long ret = prctl(PR_GHOSTMEM_FREE, 0, (unsigned long)base, 0, 0);
    if (ret < 0)
        munmap(base, PAGE_ALIGN(size));
}

/*
 * 段布局计算：找出所有 PT_LOAD 的覆盖范围。
 * 返回 (base_addr, size)，size 为页对齐后的总大小。
 */
static int compute_layout(const Elf64_Ehdr *eh, size_t elf_size,
                          Elf64_Addr *out_base_addr, size_t *out_size)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)eh + eh->e_phoff);
    Elf64_Addr base = (Elf64_Addr)-1;
    Elf64_Addr end = 0;
    int i;

    if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(Elf64_Phdr))
        return -EINVAL;

    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_vaddr < base)
            base = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > end)
            end = ph[i].p_vaddr + ph[i].p_memsz;
        /* 文件越界保护 */
        if (ph[i].p_offset + ph[i].p_filesz > elf_size)
            return -EINVAL;
    }
    if (base == (Elf64_Addr)-1)
        return -ENOEXEC;

    *out_base_addr = base;
    *out_size = PAGE_ALIGN(end - base);
    return 0;
}

/* 拷贝 PT_LOAD 段（含 BSS 清零） */
static void load_segments(const Elf64_Ehdr *eh, unsigned char *mem, Elf64_Addr base)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)eh + eh->e_phoff);
    int i;

    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        memcpy(mem + (ph[i].p_vaddr - base),
               (const char *)eh + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset(mem + (ph[i].p_vaddr - base) + ph[i].p_filesz, 0,
                   ph[i].p_memsz - ph[i].p_filesz);
        }
    }
}

/* 定位 PT_DYNAMIC 段 */
static const Elf64_Dyn *find_dynamic(const Elf64_Ehdr *eh)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)eh + eh->e_phoff);
    int i;

    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC)
            return (const Elf64_Dyn *)((const char *)eh + ph[i].p_offset);
    }
    return NULL;
}

/*
 * 符号解析：本模块内部符号直接 base+st_value；未定义符号走宿主回调。
 */
static int resolve_symbol(const Elf64_Sym *sym, const char *strtab,
                          unsigned char *mem, Elf64_Addr base,
                          const struct gh_linker_cb *cb, void **out)
{
    const char *name;

    if (!sym || !out)
        return -EINVAL;

    if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
        *out = mem + (sym->st_value - base);
        return 0;
    }

    if (sym->st_name == 0 || !strtab)
        return -ENOENT;
    name = strtab + sym->st_name;

    if (cb && cb->resolve)
        return cb->resolve(name, out, cb->udata);
    return -ENOENT;
}

/* 应用一处 RELA 重定位 */
static int apply_rela(const Elf64_Rela *rela, const Elf64_Sym *symtab,
                      const char *strtab, unsigned char *mem, Elf64_Addr base,
                      const struct gh_linker_cb *cb)
{
    Elf64_Addr *where = (Elf64_Addr *)(mem + (rela->r_offset - base));
    unsigned int type = (unsigned int)ELF64_R_TYPE(rela->r_info);
    unsigned int symidx = (unsigned int)ELF64_R_SYM(rela->r_info);
    const Elf64_Sym *sym = symidx ? &symtab[symidx] : NULL;
    void *sym_addr = NULL;
    int ret;

    switch (type) {
    case R_AARCH64_RELATIVE:
        /* B + A */
        *where = base + rela->r_addend;
        return 0;
    case R_AARCH64_ABS64:
        /* S + A */
        ret = resolve_symbol(sym, strtab, mem, base, cb, &sym_addr);
        if (ret < 0)
            return ret;
        *where = (Elf64_Addr)sym_addr + rela->r_addend;
        return 0;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
        /* S */
        ret = resolve_symbol(sym, strtab, mem, base, cb, &sym_addr);
        if (ret < 0)
            return ret;
        *where = (Elf64_Addr)sym_addr;
        return 0;
    /* ---- 宿主测试支持（x86_64）----
     * 目标平台是 ARM64；以下类型仅用于在 x86_64 宿主机上验证
     * 加载/重定位逻辑（mini.so 由 host gcc 产出）。 */
    case R_X86_64_RELATIVE:
        /* B + A，x86 的 addend 在槽位内 */
        *where = base + *where;
        return 0;
    case R_X86_64_64:
        ret = resolve_symbol(sym, strtab, mem, base, cb, &sym_addr);
        if (ret < 0)
            return ret;
        *where = (Elf64_Addr)sym_addr + *where;
        return 0;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
        ret = resolve_symbol(sym, strtab, mem, base, cb, &sym_addr);
        if (ret < 0)
            return ret;
        *where = (Elf64_Addr)sym_addr;
        return 0;
    default:
        /* v1 不支持的（TLS/IFUNC 等）显式失败，不做猜测 */
        fprintf(stderr, "ghostlinker: unsupported reloc type %u\n", type);
        return -ENOTSUP;
    }
}

/*
 * 遍历动态段应用全部 RELA 重定位，收集 init_array。
 */
static int process_dynamic(const Elf64_Dyn *dyn, unsigned char *mem,
                           Elf64_Addr base, const struct gh_linker_cb *cb,
                           struct gh_linker_result *out)
{
    const Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    const Elf64_Rela *rela = NULL;
    size_t rela_count = 0, rela_size = 0;
    int i;
    int ret = 0;

    for (i = 0; dyn[i].d_tag != DT_NULL; i++) {
        /* 动态表项 d_ptr 是 VMA，翻译为已加载内存地址 */
        switch (dyn[i].d_tag) {
        case DT_SYMTAB: symtab = (const Elf64_Sym *)(mem + (dyn[i].d_un.d_ptr - base)); break;
        case DT_STRTAB: strtab = (const char *)(mem + (dyn[i].d_un.d_ptr - base)); break;
        case DT_RELA:   rela = (const Elf64_Rela *)(mem + (dyn[i].d_un.d_ptr - base)); break;
        case DT_RELASZ: rela_size = dyn[i].d_un.d_val; break;
        case DT_RELACOUNT: rela_count = dyn[i].d_un.d_val; break;
        case DT_INIT_ARRAY:
            out->init_array = (void **)(mem + (dyn[i].d_un.d_ptr - base));
            break;
        case DT_INIT_ARRAYSZ:
            out->nr_init = (int)(dyn[i].d_un.d_val / sizeof(void *));
            break;
        default:
            break;
        }
    }

    if (!symtab || !strtab || !rela) {
        fprintf(stderr, "ghostlinker: missing DYNAMIC entries (symtab/strtab/rela)\n");
        return -ENOEXEC;
    }

    if (rela_count == 0 && rela_size)
        rela_count = rela_size / sizeof(Elf64_Rela);

    for (i = 0; i < (int)rela_count; i++) {
        ret = apply_rela(&rela[i], symtab, strtab, mem, base, cb);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int gh_link_elf(const void *elf, size_t elf_size, const struct gh_linker_cb *cb,
                struct gh_linker_result *out)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    const Elf64_Dyn *dyn;
    gh_alloc_fn alloc = cb ? cb->alloc : NULL;
    void *alloc_udata = cb ? cb->alloc_udata : NULL;
    Elf64_Addr base_addr;
    size_t size;
    unsigned char *mem;
    int ret;

    if (!elf || elf_size < sizeof(Elf64_Ehdr) || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64)
        return -ENOEXEC;
    if (eh->e_type != ET_DYN) {
        fprintf(stderr, "ghostlinker: only ET_DYN supported\n");
        return -ENOEXEC;
    }

    ret = compute_layout(eh, elf_size, &base_addr, &size);
    if (ret < 0)
        return ret;

    if (!alloc)
        alloc = default_alloc;
    mem = alloc(size, alloc_udata);
    if (!mem)
        return -ENOMEM;
    memset(mem, 0, size);

    load_segments(eh, mem, base_addr);

    dyn = find_dynamic(eh);
    if (!dyn) {
        fprintf(stderr, "ghostlinker: no PT_DYNAMIC\n");
        ret = -ENOEXEC;
        goto err;
    }

    ret = process_dynamic(dyn, mem, base_addr, cb, out);
    if (ret < 0)
        goto err;

    out->base = mem;
    out->size = size;
    return 0;

err:
    if (mem)
        default_free(mem, size);
    return ret;
}

void gh_link_free(struct gh_linker_result *res)
{
    if (!res || !res->base)
        return;
    default_free(res->base, res->size);
    res->base = NULL;
}
