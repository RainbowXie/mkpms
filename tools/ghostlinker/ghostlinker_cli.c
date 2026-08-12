/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostLinker CLI - 加载 ELF 到幽灵内存的最小演示工具。
 *
 * 用法:
 *   ghostlinker <file.so>                 # 加载并打印基址/init_array
 *   ghostlinker <file.so> --run           # 加载并调用 init_array
 *
 * 内存来源：ghostmem 内核模块 prctl（未加载时 mmap 回退）。
 */

#include "ghostlinker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int resolve_demo(const char *name, void **out_addr, void *udata)
{
    /* 演示：只报告未解析符号，不猜测地址 */
    (void)udata;
    fprintf(stderr, "ghostlinker: unresolved symbol: %s\n", name);
    return -1;
}

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
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = (size_t)sz;
    return buf;
}

int main(int argc, char *argv[])
{
    struct gh_linker_cb cb = { .resolve = resolve_demo };
    struct gh_linker_result res;
    void *elf;
    size_t elf_size;
    int run = 0;
    int ret;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--run") == 0)
            run = 1;
    }

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.so> [--run]\n", argv[0]);
        return 1;
    }

    elf = read_file(argv[1], &elf_size);
    if (!elf) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    ret = gh_link_elf(elf, elf_size, &cb, &res);
    free(elf);
    if (ret < 0) {
        fprintf(stderr, "gh_link_elf failed: %d\n", ret);
        return 1;
    }

    printf("loaded: base=%p size=%zu init_array=%p nr_init=%d\n",
           res.base, res.size, res.init_array, res.nr_init);

    if (run && res.init_array && res.nr_init > 0) {
        for (i = 0; i < res.nr_init; i++) {
            void (*fn)(void) = res.init_array[i];
            if (fn) {
                printf("calling init_array[%d] = %p\n", i, fn);
                fn();
            }
        }
    }

    gh_link_free(&res);
    return 0;
}
