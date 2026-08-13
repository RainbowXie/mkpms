/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ghostlinker BSS 清零路径测试：p_memsz > p_filesz 段应被 memset 清零。 */
#include <stdio.h>
#include <stdlib.h>
#include "ghostlinker.h"

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(int argc, char *argv[])
{
    struct gh_linker_cb cb = { 0 };
    struct gh_linker_result res;
    FILE *f = fopen(argv[1], "rb");
    long sz; void *buf;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);

    CHECK(gh_link_elf(buf, sz, &cb, &res) == 0, "load bss payload");
    if (res.base) {
        int *bss = (int *)gh_link_find_symbol(&res, "g_bss");
        int (*sum)(void) = (int (*)(void))gh_link_find_symbol(&res, "exported_bss_sum");
        CHECK(bss != NULL && sum != NULL, "bss symbols found");
        if (bss && sum) {
            int nz = 0;
            for (int i = 0; i < 256; i++) if (bss[i]) nz++;
            CHECK(nz == 0, "g_bss zeroed (BSS memset)");
            CHECK(sum() == 0, "exported_bss_sum() == 0");
        }
        gh_link_free(&res);
    }
    free(buf);
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
