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
