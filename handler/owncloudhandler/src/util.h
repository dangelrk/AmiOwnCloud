/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef OWNCLOUDHANDLER_UTIL_H
#define OWNCLOUDHANDLER_UTIL_H

/*
 * util.h -- mini libc replacement + BCPL/path helpers for the CLOUD: handler.
 *
 * The handler is linked -nostdlib (see handler.c and ../README.md): no
 * vc.lib, no malloc, no string.h. Every str-family and mem-family routine
 * the handler needs is implemented here instead. actions.c and vol.c must
 * not call any libc function -- only these, plus exec.library via
 * proto/exec.h.
 */

#include <exec/types.h>
#include <dos/dos.h>

/* ---- mini mem/str routines --------------------------------------- */
void  u_memcpy(void *dst, const void *src, ULONG n);
void  u_memset(void *dst, UBYTE val, ULONG n);
ULONG u_strlen(const char *s);
/* Bounded copy; dst is always NUL-terminated within dst[0..dstsz-1]. */
void  u_strncpy(char *dst, const char *src, ULONG dstsz);
LONG  u_strcmp(const char *a, const char *b);
const char *u_strchr(const char *s, char c);

/* Latin-1 (NOT ASCII) case-insensitive compare/uppercase -- Amiga filename
 * semantics, see docs/handler-plan.md "Namen Latin-1-case-insensitiv". */
LONG  u_stricmp_latin1(const char *a, const char *b);
UBYTE u_toupper_latin1(UBYTE c);

/* ---- BCPL <-> C string helpers ------------------------------------ */
/* BPTR to a BSTR (length byte + chars, no NUL) -> NUL-terminated C string
 * in out[0..outsz-1]. bstr==0 yields an empty string. Truncates silently. */
void  bstr_to_cstr(BPTR bstr, char *out, ULONG outsz);

/* Raw BSTR length byte (0 if bstr==0). Use to detect an over-long source
 * string (e.g. ACTION_SET_COMMENT's new comment) before bstr_to_cstr()
 * would silently truncate it -- some callers need to report an error
 * instead of truncating, see RKRM DOS 14.5.4. */
ULONG bstr_len(BPTR bstr);

/* Writes a BSTR (length byte + chars, NO trailing NUL) into field, which is
 * field_size bytes total (e.g. fib_FileName[108] or fib_Comment[80]).
 * Content is clamped to field_size-1 chars per the BSTR length byte
 * convention (RKRM DOS 14.3.1: fib_FileName/fib_Comment are BSTRs at the
 * packet level, converted to C strings by dos.library for the caller). */
void  fill_bstr_field(char *field, ULONG field_size, const char *cstr);

/* ---- path component tokenizer -------------------------------------- */
/* Advances *path past the next '/'-delimited component and copies it into
 * comp (bounded by compsz). *is_parent is set to 1 if the component was
 * empty (leading '/', or "//") -- Amiga's "empty component means go to the
 * parent directory" convention, see docs/handler-plan.md "Pfadauflösung".
 * Returns 0 once *path is exhausted (nothing left to extract). */
int   path_next_component(const char **path, char *comp, ULONG compsz, int *is_parent);

#endif /* OWNCLOUDHANDLER_UTIL_H */
