/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ghostmem init args hex 解析 host 单测。
 * 编译：gcc -Ikpms/ghostmem -o /tmp/gh_parse_test kpms/ghostmem/tests/ghostmem_args_test.c
 */
#include <stdio.h>
#include <string.h>
#include "ghostmem_args.h"

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("ok: %s\n", msg); } } while (0)

int main(void)
{
    unsigned long v;

    CHECK(gh_parse_hex("100000000", &v) == 9 && v == 0x100000000UL, "hex 4GB");
    CHECK(gh_parse_hex("0x7000000000", &v) == 12 && v == 0x7000000000UL, "hex with 0x prefix");
    CHECK(gh_parse_hex("0x", &v) == 0, "bare 0x fails");
    CHECK(gh_parse_hex("ABC", &v) == 3 && v == 0xABCUL, "uppercase hex");
    CHECK(gh_parse_hex("deadbeef", &v) == 8 && v == 0xdeadbeefUL, "lowercase hex");
    CHECK(gh_parse_hex("", &v) == 0, "empty fails");
    CHECK(gh_parse_hex("zzz", &v) == 0, "non-hex fails");
    CHECK(gh_parse_hex("12G", &v) == 2 && v == 0x12UL, "stops at non-hex");

    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
