/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ghostlinker 宿主符号解析回调测试：外部符号经 cb->resolve 注入并生效。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ghostlinker.h"

static int resolved_count = 0;
static int ext_val = 0xCAFE;
static int my_resolver(const char *name, void **out, void *udata)
{
    (void)udata;
    if (strcmp(name, "external_value") == 0) {
        *out = &ext_val;
        resolved_count++;
        return 0;
    }
    return -1; /* 其余外部符号不解析（slot=0 继续） */
}

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } \
                         else { printf("ok: %s\n", m); } } while (0)

int main(int argc, char *argv[])
{
    struct gh_linker_cb cb = { .resolve = my_resolver };
    struct gh_linker_result res;
    FILE *f = fopen(argv[1], "rb");
    long sz; void *buf;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);

    CHECK(gh_link_elf(buf, sz, &cb, &res) == 0, "load ext-symbol payload");
    CHECK(resolved_count == 1, "resolver invoked for external_value");
    if (res.base) {
        int (*get_ext)(void) = (int (*)(void))gh_link_find_symbol(&res, "exported_get_ext");
        CHECK(get_ext != NULL, "exported_get_ext found");
        if (get_ext)
            CHECK(get_ext() == 0xCAFE, "exported_get_ext() == 0xcafe (resolved via callback)");
        gh_link_free(&res);
    }
    free(buf);
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
