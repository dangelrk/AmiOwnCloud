/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * test_pairs.c -- host round-trip regression test for AmiCloudSync's
 * pairs.c (apps/amicloud-sync-cx/src/pairs.c). Compiles the REAL
 * project sources (pairs.c + libcloudcore's config.c) natively, no
 * PLATFORM_AMIGA, no cross-toolchain -- just system cc. Not part of any
 * Amiga Makefile.amiga-cross's OWN_SRCS, so it's never pulled into the
 * AmiCloudSync/AmiCloudSyncPrefs binaries.
 *
 * Written to reproduce a reported bug: "several folder pairs can be
 * added in AmiCloudSyncPrefs, but loading only returns the first one,
 * and after saving only one remains in the file." Found and fixed:
 * pairs_load()'s line[]/val[] buffers (and config_load()'s, same bug)
 * were sized far smaller than AC_MAX_PATH/AC_MAX_URL, the very fields
 * they parse. A value longer than the buffer makes fgets() stop
 * mid-line; the next fgets() call then resumes with the leftover raw
 * text as if it were a new line, which fails the "key = val" scan and
 * is silently dropped -- taking the pair (and sometimes the next real
 * line too) with it. See pairs.c/config.c for the fix and full comment.
 *
 * Run: scripts/test_pairs.sh
 */
#include "pairs.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

static void dump_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[4096];
    int pair_lines = 0;

    printf("--- file contents: %s ---\n", path);
    if (!fp) { printf("(could not open)\n"); return; }
    while (fgets(line, sizeof(line), fp)) {
        printf("%s", line);
        if (strncmp(line, "pair=", 5) == 0) pair_lines++;
    }
    fclose(fp);
    printf("--- end (pair= lines counted: %d) ---\n\n", pair_lines);
}

/* Scenario 1: realistic short pairs -- the shape AmiCloudSyncPrefs users
 * actually create (short Amiga volume:dir paths, short WebDAV paths). */
static int test_short_pairs(void)
{
    const char *path = "/tmp/test_amicloudsync_pairs_short.cfg";
    AcConfig cfg_out, cfg_in;
    PairList pl_out, pl_in;
    int before = failures;

    printf("=== scenario: short, realistic pairs ===\n");
    remove(path);

    memset(&cfg_out, 0, sizeof(cfg_out));
    strcpy(cfg_out.server, "https://cloud.example.com/remote.php/dav/files/USER");
    strcpy(cfg_out.username, "USER");
    strcpy(cfg_out.password, "app-password");
    cfg_out.verify_ssl = 1;

    pairs_init(&pl_out);
    pl_out.interval_minutes = 15;
    pairs_add(&pl_out, "Work:Dokumente", "/Dokumente", 1);
    pairs_add(&pl_out, "Work:Projekte/AmiCloud", "/Projekte/AmiCloud", 1);
    pairs_add(&pl_out, "RAM:Scratch", "/Scratch", 0);

    printf("built PairList with %d pairs (expected 3)\n", pl_out.n);
    CHECK(pairs_save(path, &cfg_out, &pl_out) == 1, "pairs_save() returns success");
    dump_file(path);

    memset(&cfg_in, 0, sizeof(cfg_in));
    pairs_load(path, &cfg_in, &pl_in);
    printf("loaded PairList with %d pairs (expected 3)\n\n", pl_in.n);
    CHECK(pl_in.n == 3, "pairs_load() returns all 3 short pairs");
    if (pl_in.n >= 3) {
        CHECK(strcmp(pl_in.items[0].local, "Work:Dokumente") == 0, "short pair[0].local matches");
        CHECK(strcmp(pl_in.items[1].local, "Work:Projekte/AmiCloud") == 0, "short pair[1].local matches");
        CHECK(strcmp(pl_in.items[2].local, "RAM:Scratch") == 0, "short pair[2].local matches");
        CHECK(pl_in.items[2].enabled == 0, "short pair[2].enabled matches (off)");
    }

    pairs_free(&pl_out);
    pairs_free(&pl_in);
    remove(path);
    return failures == before;
}

/* Scenario 2: one deliberately long pair (> the old line[768]/val[704]
 * buffers in pairs.c, and > config.c's old line[512]/val[448] for
 * server=) sandwiched between two short ones -- the buffer-desync
 * reproduction. Also re-saves the loaded result (what USE/SAVE does)
 * to confirm a load->save->load cycle doesn't lose anything either. */
static int test_long_pair_and_resave(void)
{
    const char *path = "/tmp/test_amicloudsync_pairs_long.cfg";
    AcConfig cfg_out, cfg_in;
    PairList pl_out, pl_in;
    char long_local[600], long_remote[600];
    int before = failures;

    printf("=== scenario: long pair + resave round-trip ===\n");
    remove(path);

    memset(&cfg_out, 0, sizeof(cfg_out));
    strcpy(cfg_out.server, "https://cloud.example.com/remote.php/dav/files/USER");
    strcpy(cfg_out.username, "USER");
    strcpy(cfg_out.password, "app-password");
    cfg_out.verify_ssl = 1;

    pairs_init(&pl_out);
    pl_out.interval_minutes = 15;

    memset(long_local, 'L', sizeof(long_local) - 1);
    long_local[sizeof(long_local) - 1] = '\0';
    memcpy(long_local, "Work:VeryLong/", 14); /* keep it path-ish at the front */
    memset(long_remote, 'R', sizeof(long_remote) - 1);
    long_remote[sizeof(long_remote) - 1] = '\0';
    memcpy(long_remote, "/VeryLong/", 10);

    pairs_add(&pl_out, "Work:Dokumente", "/Dokumente", 1);
    pairs_add(&pl_out, long_local, long_remote, 1);
    pairs_add(&pl_out, "RAM:Projekte", "/Projekte", 0);

    printf("built PairList with %d pairs (expected 3)\n", pl_out.n);
    CHECK(pairs_save(path, &cfg_out, &pl_out) == 1, "pairs_save() returns success");
    dump_file(path);

    memset(&cfg_in, 0, sizeof(cfg_in));
    pairs_load(path, &cfg_in, &pl_in);
    printf("loaded PairList with %d pairs (expected 3)\n\n", pl_in.n);

    CHECK(pl_in.n == 3, "pairs_load() returns all 3 pairs (incl. long one)");
    CHECK(strcmp(cfg_in.server, cfg_out.server) == 0, "server round-trips");
    CHECK(strcmp(cfg_in.username, cfg_out.username) == 0, "username round-trips");
    CHECK(pl_in.interval_minutes == 15, "interval round-trips");

    if (pl_in.n >= 1) {
        CHECK(strcmp(pl_in.items[0].local, "Work:Dokumente") == 0, "pair[0].local matches");
        CHECK(strcmp(pl_in.items[0].remote, "/Dokumente") == 0, "pair[0].remote matches");
        CHECK(pl_in.items[0].enabled == 1, "pair[0].enabled matches");
    }
    if (pl_in.n >= 2) {
        CHECK(strcmp(pl_in.items[1].local, long_local) == 0, "pair[1].local (long) matches");
        CHECK(strcmp(pl_in.items[1].remote, long_remote) == 0, "pair[1].remote (long) matches");
    }
    if (pl_in.n >= 3) {
        CHECK(strcmp(pl_in.items[2].local, "RAM:Projekte") == 0, "pair[2].local matches");
        CHECK(pl_in.items[2].enabled == 0, "pair[2].enabled matches (off)");
    }

    printf("\n--- second round-trip (load -> save -> load, what USE/SAVE does) ---\n");
    CHECK(pairs_save(path, &cfg_in, &pl_in) == 1, "second pairs_save() returns success");
    dump_file(path);

    {
        AcConfig cfg_in2;
        PairList pl_in2;
        memset(&cfg_in2, 0, sizeof(cfg_in2));
        pairs_load(path, &cfg_in2, &pl_in2);
        printf("reloaded PairList with %d pairs (expected 3)\n\n", pl_in2.n);
        CHECK(pl_in2.n == 3, "second round-trip still has all 3 pairs");
        pairs_free(&pl_in2);
    }

    pairs_free(&pl_out);
    pairs_free(&pl_in);
    remove(path);
    return failures == before;
}

int main(void)
{
    test_short_pairs();
    test_long_pair_and_resave();

    printf("\n%s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
