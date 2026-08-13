/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostLinker host unit test - 真实验证重定位与符号查找。
 *
 * 编译并运行:
 *   gcc -shared -fPIC -o /tmp/ghl_payload.so -x c - <<'EOF'
 *   int g_value = 42; int *g_ptr = &g_value;
 *   int exported_add(int a, int b) { return a + b; }
 *   EOF
 *   gcc -o /tmp/ghl_test ghostlinker.c ghostlinker_test.c -I.
 *   /tmp/ghl_test /tmp/ghl_payload.so
 *
 * v1 语义：仅验证段装载 + RELATIVE/ABS64 数据重定位 + 符号查找。
 * 不含外部依赖的纯数据 payload 不调用（PLT stub 未填充，v1 非目标）。
 * 返回 0 全部通过；非 0 有失败。
 */
#include "ghostlinker.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

static void *read_file(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    void *buf;

    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return NULL; }
    buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_size = (size_t)sz;
    return buf;
}

int main(int argc, char *argv[])
{
    struct gh_linker_cb cb = { 0 };
    struct gh_linker_result res;
    void *elf;
    size_t elf_size;
    int ret;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <payload.so>\n", argv[0]);
        return 2;
    }

    /* 1. 拒绝非 ELF 输入（大小够 Ehdr 但魔数错误） */
    {
        char fake[64];
        memset(fake, 'A', sizeof(fake));
        ret = gh_link_elf(fake, sizeof(fake), &cb, &res);
        CHECK(ret == -ENOEXEC, "reject non-ELF input");
    }

    /* 1b. 错误路径：截断 / 坏 e_type / 缺 PT_LOAD */
    {
        /* 截断（不足 Ehdr） */
        char tiny[8];
        ret = gh_link_elf(tiny, sizeof(tiny), &cb, &res);
        CHECK(ret == -EINVAL, "truncated input -> EINVAL");

        /* 合法 ELF 魔数但 e_type 非法 */
        Elf64_Ehdr eh;
        memset(&eh, 0, sizeof(eh));
        memcpy(eh.e_ident, ELFMAG, SELFMAG);
        eh.e_ident[EI_CLASS] = ELFCLASS64;
        eh.e_type = ET_EXEC; /* 仅支持 ET_DYN */
        ret = gh_link_elf(&eh, sizeof(eh), &cb, &res);
        CHECK(ret == -ENOEXEC, "ET_EXEC rejected (only ET_DYN)");

        /* ET_DYN 但无 PT_LOAD（e_phnum=0 → compute_layout -EINVAL） */
        eh.e_type = ET_DYN;
        ret = gh_link_elf(&eh, sizeof(eh), &cb, &res);
        CHECK(ret == -EINVAL, "no PT_LOAD -> EINVAL");

        /* 构造：ET_DYN + PT_LOAD 但无 PT_DYNAMIC → -ENOEXEC */
        {
            unsigned char buf[256];
            Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
            Elf64_Phdr *ph = (Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr));
            memset(buf, 0, sizeof(buf));
            memcpy(e->e_ident, ELFMAG, SELFMAG);
            e->e_ident[EI_CLASS] = ELFCLASS64;
            e->e_type = ET_DYN;
            e->e_phoff = sizeof(Elf64_Ehdr);
            e->e_phentsize = sizeof(Elf64_Phdr);
            e->e_phnum = 1;
            ph[0].p_type = PT_LOAD;
            ph[0].p_vaddr = 0;
            ph[0].p_offset = 0;
            ph[0].p_filesz = 0x100;
            ph[0].p_memsz = 0x100;
            ret = gh_link_elf(buf, sizeof(buf), &cb, &res);
            CHECK(ret == -ENOEXEC, "no PT_DYNAMIC -> ENOEXEC");
        }

        /* 构造：PT_DYNAMIC 存在但缺 SYMTAB/STRTAB/RELA → process_dynamic -ENOEXEC */
        {
            unsigned char buf[512];
            Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
            Elf64_Phdr *ph = (Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr));
            /* 布局：Ehdr + Phdr(2) + dyn 数组；dyn 的 d_ptr 用相对 VMA */
            unsigned long dyn_off = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
            Elf64_Dyn *dyn = (Elf64_Dyn *)(buf + dyn_off);
            memset(buf, 0, sizeof(buf));
            memcpy(e->e_ident, ELFMAG, SELFMAG);
            e->e_ident[EI_CLASS] = ELFCLASS64;
            e->e_type = ET_DYN;
            e->e_phoff = sizeof(Elf64_Ehdr);
            e->e_phentsize = sizeof(Elf64_Phdr);
            e->e_phnum = 2;
            /* PT_LOAD：覆盖整个文件（含 dyn 数组） */
            ph[0].p_type = PT_LOAD;
            ph[0].p_vaddr = 0;
            ph[0].p_offset = 0;
            ph[0].p_filesz = 512;
            ph[0].p_memsz = 512;
            /* PT_DYNAMIC：指向只有 DT_NULL 的数组（无 SYMTAB/STRTAB/RELA） */
            ph[1].p_type = PT_DYNAMIC;
            ph[1].p_offset = dyn_off;
            ph[1].p_vaddr = dyn_off; /* VMA == 文件偏移（load base=0） */
            dyn[0].d_tag = DT_NULL;
            dyn[0].d_un.d_val = 0;
            ret = gh_link_elf(buf, sizeof(buf), &cb, &res);
            CHECK(ret == -ENOEXEC, "missing DYNAMIC symtab/strtab/rela -> ENOEXEC");
        }

        /* 构造：RELA 含未知 reloc type → apply_rela -ENOTSUP */
        {
            unsigned char buf[1024];
            Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
            Elf64_Phdr *ph = (Elf64_Phdr *)(buf + sizeof(Elf64_Ehdr));
            unsigned long sym_off = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
            unsigned long str_off = sym_off + sizeof(Elf64_Sym) * 2;
            unsigned long rela_off = str_off + 32;
            unsigned long dyn_off = rela_off + sizeof(Elf64_Rela);
            Elf64_Sym *sym = (Elf64_Sym *)(buf + sym_off);
            char *strtab = (char *)(buf + str_off);
            Elf64_Rela *rela = (Elf64_Rela *)(buf + rela_off);
            Elf64_Dyn *dyn = (Elf64_Dyn *)(buf + dyn_off);
            int i;
            memset(buf, 0, sizeof(buf));
            memcpy(e->e_ident, ELFMAG, SELFMAG);
            e->e_ident[EI_CLASS] = ELFCLASS64;
            e->e_type = ET_DYN;
            e->e_phoff = sizeof(Elf64_Ehdr);
            e->e_phentsize = sizeof(Elf64_Phdr);
            e->e_phnum = 2;
            ph[0].p_type = PT_LOAD;
            ph[0].p_vaddr = 0;
            ph[0].p_offset = 0;
            ph[0].p_filesz = 1024;
            ph[0].p_memsz = 1024;
            ph[1].p_type = PT_DYNAMIC;
            ph[1].p_offset = dyn_off;
            ph[1].p_vaddr = dyn_off;
            /* 符号表：2 项（含 1 个定义符号供 RELATIVE 用） */
            sym[0].st_name = 0;
            sym[1].st_name = 1; /* 指向 strtab 偏移 1 */
            sym[1].st_value = 0x100;
            sym[1].st_shndx = 1; /* 非 UNDEF → 内部符号 */
            strtab[1] = 'x'; /* 符号名（仅非空即可） */
            /* RELA：1 条未知 type（0x9999 无符号） */
            rela[0].r_offset = 0x100;
            rela[0].r_info = ((unsigned long)1 << 32) | 0x9999UL;
            rela[0].r_addend = 0;
            /* DYNAMIC：SYMTAB/STRTAB/RELA/RELASZ/NULL */
            i = 0;
            dyn[i].d_tag = DT_SYMTAB; dyn[i].d_un.d_ptr = sym_off; i++;
            dyn[i].d_tag = DT_STRTAB; dyn[i].d_un.d_ptr = str_off; i++;
            dyn[i].d_tag = DT_RELA; dyn[i].d_un.d_ptr = rela_off; i++;
            dyn[i].d_tag = DT_RELASZ; dyn[i].d_un.d_val = sizeof(Elf64_Rela); i++;
            dyn[i].d_tag = DT_NULL; i++;
            ret = gh_link_elf(buf, sizeof(buf), &cb, &res);
            CHECK(ret == -ENOTSUP, "unknown reloc type -> ENOTSUP");
        }
    }

    /* 2. 真实加载 */
    elf = read_file(argv[1], &elf_size);
    CHECK(elf != NULL, "read payload .so");
    if (!elf)
        return 1;

    ret = gh_link_elf(elf, elf_size, &cb, &res);
    free(elf);
    CHECK(ret == 0, "gh_link_elf succeeded");
    if (ret != 0)
        return 1;
    CHECK(res.base != NULL && res.size > 0, "mapping allocated");

    /* 3. 符号查找（主动调用能力的数据面） */
    {
        int *g_value_p = (int *)gh_link_find_symbol(&res, "g_value");
        int **g_ptr_p = (int **)gh_link_find_symbol(&res, "g_ptr");
        CHECK(g_value_p != NULL, "find g_value");
        CHECK(g_ptr_p != NULL, "find g_ptr");
        if (g_value_p)
            CHECK(*g_value_p == 42, "g_value == 42 (data loaded)");
        /* g_ptr 经 RELATIVE/ABS64 重定位后应指向 g_value */
        if (g_value_p && g_ptr_p)
            CHECK(*g_ptr_p == g_value_p, "g_ptr -> g_value (reloc ok)");
    }

    /* 4. 内部函数符号可寻址且可调用（exported_add 无外部依赖，直接验证重定位） */
    {
        int (*add_fn)(int, int) =
            (int (*)(int, int))gh_link_find_symbol(&res, "exported_add");
        CHECK(add_fn != NULL, "exported_add found");
        if (add_fn) {
            CHECK(add_fn(7, 8) == 15, "call exported_add(7,8) == 15");
            CHECK(add_fn(-3, 10) == 7, "call exported_add(-3,10) == 7");
        }
        CHECK(gh_link_find_symbol(&res, NULL) == NULL, "find with NULL name");
        CHECK(gh_link_find_symbol(NULL, "x") == NULL, "find with NULL res");
    }

    /* 5. 不存在的符号返回 NULL */
    CHECK(gh_link_find_symbol(&res, "nonexistent_sym") == NULL,
          "find nonexistent returns NULL");

    /* 6. init_array：只调用导出构造器 my_ctor，验证副作用 g_ctor_flag */
    {
        int *flag = (int *)gh_link_find_symbol(&res, "g_ctor_flag");
        int called = 0, i;
        CHECK(flag != NULL, "find g_ctor_flag");
        if (res.init_array && res.nr_init > 0) {
            for (i = 0; i < res.nr_init; i++) {
                void (*fn)(void) = res.init_array[i];
                int j;
                if (!fn)
                    continue;
                for (j = 0; j < res.sym_count; j++) {
                    const Elf64_Sym *s = &res.symtab[j];
                    if (s->st_value &&
                        (char *)res.base + (s->st_value - res.load_base) == (char *)fn &&
                        res.strtab && s->st_name &&
                        strcmp(res.strtab + s->st_name, "my_ctor") == 0) {
                        fn();
                        called = 1;
                        break;
                    }
                }
            }
        }
        CHECK(called, "init_array invokes exported ctor");
        if (flag)
            CHECK(*flag == 0xCAFE, "ctor side effect g_ctor_flag == 0xCAFE");
    }

    gh_link_free(&res);
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
