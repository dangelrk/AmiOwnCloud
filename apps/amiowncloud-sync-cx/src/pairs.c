/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * pairs.c -- see pairs.h. Manual key=value / pipe-split parsing,
 * matching the rest of this codebase's style (cmd.c's url_encode(),
 * webdav.c's build_url()) rather than pulling in strtok()/regex for a
 * handful of short, fixed-shape lines.
 */
#include "pairs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pairs_init(PairList *pl)
{
    pl->items = NULL;
    pl->n = 0;
    pl->cap = 0;
    pl->interval_minutes = 15; /* plan default */
}

void pairs_add(PairList *pl, const char *local, const char *remote, int enabled)
{
    SyncPair *p;
    if (pl->n == pl->cap) {
        int newcap = pl->cap ? pl->cap * 2 : 8;
        SyncPair *tmp = (SyncPair *)realloc(pl->items, sizeof(SyncPair) * (size_t)newcap);
        if (!tmp) return;
        pl->items = tmp;
        pl->cap = newcap;
    }
    p = &pl->items[pl->n++];
    strncpy(p->local, local ? local : "", sizeof(p->local) - 1);
    p->local[sizeof(p->local) - 1] = '\0';
    strncpy(p->remote, remote ? remote : "", sizeof(p->remote) - 1);
    p->remote[sizeof(p->remote) - 1] = '\0';
    p->enabled = enabled;
}

void pairs_remove(PairList *pl, int index)
{
    int i;
    if (index < 0 || index >= pl->n) return;
    for (i = index; i < pl->n - 1; i++) pl->items[i] = pl->items[i + 1];
    pl->n--;
}

void pairs_free(PairList *pl)
{
    free(pl->items);
    pl->items = NULL;
    pl->n = pl->cap = 0;
}

/* Same leading/trailing-whitespace strip as libcloudcore's config.c
 * (not exported from there -- this is a short enough helper to just
 * duplicate rather than widen config.h's API for one internal use). */
static void strip(char *s)
{
    char *p = s + strlen(s) - 1;
    while (p >= s && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) *p-- = '\0';
    p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/* Splits "local|remote|on" (or "off"/"0"/"1") into a new pair, appended
 * to pl. Malformed lines (missing a field) are silently skipped -- a
 * hand-edited prefs file with a typo shouldn't crash the commodity. */
static void parse_pair_line(PairList *pl, const char *val)
{
    char local[AC_MAX_PATH], remote[AC_MAX_PATH], flag[16];
    const char *p1, *p2;
    size_t len;

    p1 = strchr(val, '|');
    if (!p1) return;
    len = (size_t)(p1 - val);
    if (len >= sizeof(local)) len = sizeof(local) - 1;
    memcpy(local, val, len);
    local[len] = '\0';
    strip(local);

    p2 = strchr(p1 + 1, '|');
    if (!p2) return;
    len = (size_t)(p2 - (p1 + 1));
    if (len >= sizeof(remote)) len = sizeof(remote) - 1;
    memcpy(remote, p1 + 1, len);
    remote[len] = '\0';
    strip(remote);

    strncpy(flag, p2 + 1, sizeof(flag) - 1);
    flag[sizeof(flag) - 1] = '\0';
    strip(flag);

    if (!local[0] || !remote[0]) return;
    pairs_add(pl, local, remote,
             (strcmp(flag, "on") == 0 || strcmp(flag, "1") == 0));
}

int pairs_load(const char *filepath, AcConfig *cfg, PairList *pl)
{
    FILE *fp;
    /* val must hold a whole "local|remote|flag" value -- two AC_MAX_PATH
     * paths back to back can add up to ~2*AC_MAX_PATH. Undersizing this
     * doesn't just truncate the pair: fgets() stops mid-line (no '\n'
     * yet), so the NEXT fgets() call resumes with the leftover raw path
     * text as if it were a fresh line. That fragment has no '=' in it,
     * so it fails the "key = val" scan and is silently skipped -- the
     * pair is dropped entirely, and depending on where the split lands,
     * a following real line can desync too. Confirmed on the host with
     * a >512-char pair (see tests/test_pairs.c): a 3-pair PairList came
     * back with only 2 after the round trip. Sized the same way as
     * libcloudcore's config_load(), which had the identical bug for its
     * own longest field (server=, AC_MAX_URL). */
    char line[2 * AC_MAX_PATH + 128], key[64], val[2 * AC_MAX_PATH];

    pairs_init(pl);
    config_load(filepath, cfg); /* missing file: cfg stays zeroed, not an error */

    fp = fopen(filepath, "r");
    if (!fp) return 1; /* no prefs yet -- first run */

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        /* 2047 == sizeof(val) - 1 (2*AC_MAX_PATH - 1) -- keep in sync
         * with val[]'s declaration above. */
        if (sscanf(line, " %63[^= \t] = %2047[^\n]", key, val) != 2) continue;
        strip(key);
        strip(val);

        if (strcmp(key, "interval") == 0)
            pl->interval_minutes = atoi(val);
        else if (strcmp(key, "pair") == 0)
            parse_pair_line(pl, val);
    }
    fclose(fp);
    return 1;
}

int pairs_save(const char *filepath, const AcConfig *cfg, const PairList *pl)
{
    char tmp_path[AC_MAX_PATH + 8];
    FILE *fp;
    int i;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);
    fp = fopen(tmp_path, "w");
    if (!fp) return 0;

    /* user=/pass=, not username=/password= -- config_load() accepts both,
     * but user=/pass= is the convention every other config file/example
     * in this repo (amicloud.cfg.example, AmiCloud/AMIVCS docs) already
     * uses; writing the other spelling here would be a needless drift. */
    fprintf(fp, "server=%s\n",      cfg->server);
    fprintf(fp, "user=%s\n",        cfg->username);
    fprintf(fp, "pass=%s\n",        cfg->password);
    fprintf(fp, "verify_ssl=%d\n",  cfg->verify_ssl);
    fprintf(fp, "interval=%d\n",    pl->interval_minutes);
    for (i = 0; i < pl->n; i++) {
        const SyncPair *p = &pl->items[i];
        fprintf(fp, "pair=%s|%s|%s\n", p->local, p->remote, p->enabled ? "on" : "off");
    }

    if (fclose(fp) != 0) { remove(tmp_path); return 0; }
    /* AmigaDOS Rename() fails outright if the destination already
     * exists (see dos.doc's Rename() autodoc) -- unlike POSIX rename(),
     * it never replaces an existing target. Only the very first save of
     * a given file (target doesn't exist yet) would ever succeed
     * without this; every save after that would silently leave the
     * file exactly as it was (see libcloudcore's config_save(), which
     * had the identical bug -- same fix, same reasoning). Delete the
     * old target first; if there isn't one yet (first save), this
     * simply fails harmlessly. */
    remove(filepath);
    if (rename(tmp_path, filepath) != 0) { remove(tmp_path); return 0; }
    return 1;
}
