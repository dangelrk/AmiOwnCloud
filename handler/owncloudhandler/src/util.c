/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "util.h"

/* ---- mini mem/str routines --------------------------------------- */

void u_memcpy(void *dst, const void *src, ULONG n)
{
    UBYTE *d = (UBYTE *)dst;
    const UBYTE *s = (const UBYTE *)src;
    while (n--) *d++ = *s++;
}

void u_memset(void *dst, UBYTE val, ULONG n)
{
    UBYTE *d = (UBYTE *)dst;
    while (n--) *d++ = val;
}

ULONG u_strlen(const char *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

void u_strncpy(char *dst, const char *src, ULONG dstsz)
{
    ULONG i = 0;
    if (!dstsz) return;
    while (i < dstsz - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

LONG u_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (LONG)(UBYTE)*a - (LONG)(UBYTE)*b;
}

const char *u_strchr(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return s;
    return (c == '\0') ? s : (const char *)0;
}

/* Latin-1 uppercase: ASCII a-z, plus the accented lowercase block
 * 0xE0-0xFE, excluding 0xF7 (division sign, not a letter). 0xDF (sharp s)
 * has no single-character uppercase in Latin-1 and is left alone -- same
 * behaviour as AmigaOS's international-mode ToUpper(). */
UBYTE u_toupper_latin1(UBYTE c)
{
    if (c >= 'a' && c <= 'z') return (UBYTE)(c - 0x20);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (UBYTE)(c - 0x20);
    return c;
}

LONG u_stricmp_latin1(const char *a, const char *b)
{
    UBYTE ca, cb;
    for (;;) {
        ca = u_toupper_latin1((UBYTE)*a);
        cb = u_toupper_latin1((UBYTE)*b);
        if (ca != cb) return (LONG)ca - (LONG)cb;
        if (ca == 0) return 0;
        a++; b++;
    }
}

/* ---- BCPL <-> C string helpers ------------------------------------ */

void bstr_to_cstr(BPTR bstr, char *out, ULONG outsz)
{
    UBYTE *p;
    ULONG len, i;

    if (!outsz) return;
    if (!bstr) { out[0] = '\0'; return; }

    p = (UBYTE *)BADDR(bstr);
    len = p[0];
    if (len > outsz - 1) len = outsz - 1;
    for (i = 0; i < len; i++) out[i] = (char)p[1 + i];
    out[len] = '\0';
}

ULONG bstr_len(BPTR bstr)
{
    UBYTE *p;
    if (!bstr) return 0;
    p = (UBYTE *)BADDR(bstr);
    return p[0];
}

void fill_bstr_field(char *field, ULONG field_size, const char *cstr)
{
    ULONG len = u_strlen(cstr);
    if (len > field_size - 1) len = field_size - 1;
    field[0] = (char)len;
    u_memcpy(field + 1, cstr, len);
}

/* ---- path component tokenizer -------------------------------------- */

int path_next_component(const char **path, char *comp, ULONG compsz, int *is_parent)
{
    const char *p = *path;
    const char *slash;
    ULONG len;

    if (!p || *p == '\0') return 0;

    slash = u_strchr(p, '/');
    len = slash ? (ULONG)(slash - p) : u_strlen(p);
    *is_parent = (len == 0);

    if (*is_parent) {
        comp[0] = '\0';
    } else {
        ULONG cl = len;
        if (cl > compsz - 1) cl = compsz - 1;
        u_memcpy(comp, p, cl);
        comp[cl] = '\0';
    }

    *path = slash ? slash + 1 : p + u_strlen(p);
    return 1;
}
