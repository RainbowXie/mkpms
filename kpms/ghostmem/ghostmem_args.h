/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GhostMem init args 解析（自包含，无内核依赖——host 单测可复用）。
 * 支持键值对：base=<hex> limit=<hex>，逗号/空格分隔。
 */

#ifndef _KPM_GHOSTMEM_ARGS_H_
#define _KPM_GHOSTMEM_ARGS_H_

/* 解析 "<hex>" 前缀（支持可选 0x），返回消耗字符数；非法返回 0。 */
static inline int gh_parse_hex(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    int n = 0, pre = 0;
    char c;

    /* 可选 0x / 0X 前缀 */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        pre = 2;
    }

    while ((c = *s)) {
        unsigned int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        v = (v << 4) | d;
        n++;
        s++;
    }
    if (n == 0)
        return 0;
    *out = v;
    return n + pre;
}

#endif /* _KPM_GHOSTMEM_ARGS_H_ */
