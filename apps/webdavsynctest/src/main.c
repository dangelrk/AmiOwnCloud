/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * webdavsynctest -- console test suite for the WebDAV sync transport
 * (libcloudsync/webdav.h) and the transport-neutral sync kernel
 * (cloudsync_run_t, sync.h). Runs the same scenarios that were manually
 * verified against a real OwnCloud/Nextcloud server while building this
 * tool, as an automated, repeatable regression check:
 *
 *   T1  initial sync uploads new local files
 *   T2  a second sync immediately after transfers nothing (this is the
 *       regression check for the md5/token-conflation bug found during
 *       development, which made every second sync flip between an
 *       unwanted upload and an unwanted download forever)
 *   T3  a local change triggers exactly one upload, then reconverges
 *   T4  a remote-side change (simulated via a raw PUT, bypassing the
 *       sync engine entirely -- standing in for "edited from another
 *       device") triggers exactly one download with matching content,
 *       then reconverges (regression check for the post-download md5
 *       patch, see sync.c's SA_DOWNLOAD case)
 *   T5  a local delete propagates to the server, then reconverges
 *   T6  diverging changes on both sides are reported as a conflict and
 *       resolve NEITHER side -- never silently overwritten
 *
 * Deliberately console-only (no GUI) so it cross-compiles and runs
 * identically on AmigaOS -- the same checks that ran against a real
 * server on a Mac during development can be re-run on real/emulated
 * Amiga hardware before the GUI commodity (Teil B) is trusted with it.
 *
 * Creates one scratch folder locally and one on the remote server,
 * exercises it, and removes both at the end (best-effort, even after a
 * failure) -- never touches anything outside that scratch folder. No
 * PHP, no AmiCloud/api.php anywhere in this tool -- see webdav.h.
 */
#include "amicloud.h"
#include "config.h"
#include "http.h"
#include "sync.h"
#include "webdav.h"
#include "localfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef PLATFORM_AMIGA
long __stack = 65536;
const char amiga_ver[] = "$VER: webdavsynctest " AC_VERSION " (27.09.2026)";
#endif

#ifdef PLATFORM_AMIGA
#  define RAW_TMP_PATH "T:webdavsynctest.tmp"
#elif defined(PLATFORM_WIN)
#  define RAW_TMP_PATH "webdavsynctest.tmp"
#else
#  define RAW_TMP_PATH "/tmp/webdavsynctest.tmp"
#endif

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("[PASS] %s\n", what); }
    else      { g_fail++; printf("[FAIL] %s\n", what); }
}

static int write_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return 1;
}

static int read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (!fp) return 0;
    n = fread(buf, 1, bufsz - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return 1;
}

/* Same server-URL join logic as webdav.c's own build_url() (see there)
 * -- duplicated rather than shared, since this test deliberately bypasses
 * the sync engine to act as an independent actor. The BASE it joins onto
 * is not duplicated, though: it goes through webdav_derive_base_url()
 * (same function webdav_transport_open() itself calls), so a bare-host
 * cfg->server (e.g. "https://cloud.example.com", the "nur Hauptserver"
 * AmiCloudSyncPrefs shape) still resolves to the exact same collection
 * URL here as it does for the real transport being tested. */
static void build_test_url(char *out, size_t outsz, const AcConfig *cfg, const char *remote_path)
{
    char base[AC_MAX_URL];
    size_t srvlen;
    int server_slash, path_slash;

    webdav_derive_base_url(base, sizeof(base), cfg);
    srvlen = strlen(base);
    server_slash = srvlen > 0 && base[srvlen - 1] == '/';
    path_slash   = remote_path && remote_path[0] == '/';

    if (server_slash && path_slash)
        snprintf(out, outsz, "%s%s", base, remote_path + 1);
    else if (!server_slash && !path_slash && remote_path[0])
        snprintf(out, outsz, "%s/%s", base, remote_path);
    else
        snprintf(out, outsz, "%s%s", base, remote_path);
}

static void make_test_auth(char *out, size_t outsz, const AcConfig *cfg)
{
    snprintf(out, outsz, "%s:%s", cfg->username, cfg->password);
}

/* Simulates "someone/something else changed this file" -- a raw PUT
 * straight through libcloudhttp, never touching cloudsync_run_t(). */
static int raw_put(const AcConfig *cfg, const char *remote_path, const char *content)
{
    char url[AC_MAX_URL], auth[192];
    FILE *fp;
    long len;
    HttpResponse *resp;
    int ok;

    if (!write_file(RAW_TMP_PATH, content)) return 0;
    fp = fopen(RAW_TMP_PATH, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    build_test_url(url, sizeof(url), cfg, remote_path);
    make_test_auth(auth, sizeof(auth), cfg);

    resp = http_upload_hdr("PUT", url, auth, NULL, fp, len, cfg->verify_ssl, remote_path);
    fclose(fp);
    remove(RAW_TMP_PATH);

    ok = resp && resp->status_code >= 200 && resp->status_code < 300;
    http_response_free(resp);
    return ok;
}

static int raw_delete(const AcConfig *cfg, const char *remote_path)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok;

    build_test_url(url, sizeof(url), cfg, remote_path);
    make_test_auth(auth, sizeof(auth), cfg);

    resp = http_request("DELETE", url, auth, NULL, 0, cfg->verify_ssl);
    ok = resp && resp->status_code != 0;
    http_response_free(resp);
    return ok;
}

/* status_out always receives the HTTP status (0 = network failure) so
 * callers can check "really gone" (404) as well as "really there" (200). */
static int raw_get(const AcConfig *cfg, const char *remote_path, char *buf, size_t bufsz, int *status_out)
{
    char url[AC_MAX_URL], auth[192];
    HttpResponse *resp;
    int ok = 0;

    build_test_url(url, sizeof(url), cfg, remote_path);
    make_test_auth(auth, sizeof(auth), cfg);

    resp = http_request("GET", url, auth, NULL, 0, cfg->verify_ssl);
    if (!resp) { if (status_out) *status_out = 0; return 0; }
    if (status_out) *status_out = resp->status_code;
    if (resp->status_code == 200 && resp->body) {
        size_t n = (size_t)resp->body_len;
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, resp->body, n);
        buf[n] = '\0';
        ok = 1;
    }
    http_response_free(resp);
    return ok;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "webdavsynctest v" AC_VERSION " -- console test suite for the WebDAV sync transport\n"
        "Usage: %s [options] <local-scratch-dir> <remote-scratch-path>\n"
        "\n"
        "  <remote-scratch-path> must not already exist -- it is created,\n"
        "  used, and removed again by this test (e.g. /WebdavSyncTest).\n"
        "\n"
        "Options:\n"
        "  -s <url>     WebDAV collection base URL, e.g.\n"
        "               https://host/remote.php/dav/files/USER\n"
        "  -u <user>    Username\n"
        "  -p <pass>    Password (Nextcloud: an app password)\n"
        "  -c <file>    Config file (default: ~/.webdavsyncrc or ENV:webdavsync.cfg)\n"
        "  -k           Disable TLS certificate verification (development only)\n",
        prog);
    exit(1);
}

int main(int argc, char **argv)
{
    AcConfig cfg;
    const char *opt_server = NULL, *opt_user = NULL, *opt_pass = NULL, *opt_cfgfile = NULL;
    int opt_noverify = 0;
    int i;
    const char *local_root, *remote_root;
    SyncTransport *t;
    Progress    pr;
    SyncSummary sum;
    char note1_path[AC_MAX_PATH], sub_dir[AC_MAX_PATH], note2_path[AC_MAX_PATH], baseline_path[AC_MAX_PATH];
    char remote_note1[AC_MAX_PATH], remote_note2[AC_MAX_PATH];
    char content[256];

    memset(&cfg, 0, sizeof(cfg));
    cfg.basepath[0] = '/';
    cfg.basepath[1] = '\0';
    cfg.verify_ssl  = 1;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;
        if      (strcmp(argv[i], "-s") == 0 && i+1 < argc) { opt_server  = argv[++i]; }
        else if (strcmp(argv[i], "-u") == 0 && i+1 < argc) { opt_user    = argv[++i]; }
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) { opt_pass    = argv[++i]; }
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc) { opt_cfgfile = argv[++i]; }
        else if (strcmp(argv[i], "-k") == 0)               { opt_noverify = 1; }
        else if (strcmp(argv[i], "--help") == 0)           { usage(argv[0]); }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); }
    }

    if (opt_cfgfile) {
        config_load(opt_cfgfile, &cfg);
    } else {
        char primary[AC_MAX_PATH], fallback[AC_MAX_PATH];
        config_default_path("webdavsync", primary, sizeof(primary), fallback, sizeof(fallback));
        if (!config_load(primary, &cfg) && fallback[0]) config_load(fallback, &cfg);
    }
    config_merge(&cfg, opt_server, opt_user, opt_pass, NULL);
    if (opt_noverify) cfg.verify_ssl = 0;

    if (i + 1 >= argc) { fprintf(stderr, "webdavsynctest: <local-scratch-dir> and <remote-scratch-path> required\n"); usage(argv[0]); }
    local_root  = argv[i++];
    remote_root = argv[i];

    if (!cfg.server[0])   { fprintf(stderr, "webdavsynctest: WebDAV base URL required (-s or config file)\n"); usage(argv[0]); }
    if (!cfg.username[0]) { fprintf(stderr, "webdavsynctest: username required (-u or config file)\n");        usage(argv[0]); }
    if (!cfg.password[0]) { fprintf(stderr, "webdavsynctest: password required (-p or config file)\n");        usage(argv[0]); }
    {
        size_t len = strlen(cfg.server);
        if (len > 0 && cfg.server[len-1] == '/') cfg.server[len-1] = '\0';
    }

    if (!http_init()) {
        fprintf(stderr, "webdavsynctest: network initialisation failed\n");
        return 1;
    }

    printf("webdavsynctest: %s <-> %s\n\n", local_root, remote_root);

    localfs_join(note1_path,    sizeof(note1_path),    local_root, "note1.txt");
    localfs_join(sub_dir,       sizeof(sub_dir),       local_root, "sub");
    localfs_join(note2_path,    sizeof(note2_path),    local_root, "sub/note2.txt");
    localfs_join(baseline_path, sizeof(baseline_path), local_root, ".amicloudsync");
    snprintf(remote_note1, sizeof(remote_note1), "%s/note1.txt",     remote_root);
    snprintf(remote_note2, sizeof(remote_note2), "%s/sub/note2.txt", remote_root);

    /* ---- Setup: guarantee a genuinely fresh pair on both sides ---- */
    printf("--- Setup ---\n");
    raw_delete(&cfg, remote_root); /* best-effort -- may not exist yet */
    remove(baseline_path);
    remove(note1_path);
    remove(note2_path);
    localfs_mkdir_p(local_root);
    localfs_mkdir_p(sub_dir);
    write_file(note1_path, "initial content A\n");
    write_file(note2_path, "initial content B\n");

    t = webdav_transport_open(&cfg);
    if (!t) {
        fprintf(stderr, "webdavsynctest: out of memory\n");
        http_cleanup();
        return 1;
    }

    /* ---- T1/T2: initial upload, then converge ---- */
    printf("\n--- T1/T2: initial sync + convergence ---\n");
    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.uploaded == 2 && sum.downloaded == 0 && sum.errors == 0,
         "T1: initial sync uploads both new local files");

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.uploaded == 0 && sum.downloaded == 0 && sum.unchanged == 2 && sum.errors == 0,
         "T2: second sync transfers nothing (md5/token convergence)");

    /* ---- T3: local change -> upload -> reconverge ---- */
    printf("\n--- T3: local change ---\n");
    write_file(note1_path, "locally modified content\n");
    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.uploaded == 1 && sum.downloaded == 0 && sum.errors == 0,
         "T3a: local change triggers exactly one upload");

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.uploaded == 0 && sum.downloaded == 0 && sum.errors == 0,
         "T3b: sync after upload is stable (no token-gap re-transfer)");

    /* ---- T4: remote-side change (simulated external edit) -> download ---- */
    printf("\n--- T4: remote change ---\n");
    check(raw_put(&cfg, remote_note1, "changed on the server\n"),
         "T4 setup: raw PUT simulating an external server-side edit");

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.downloaded == 1 && sum.uploaded == 0 && sum.errors == 0,
         "T4a: remote change triggers exactly one download");

    check(read_file(note1_path, content, sizeof(content)) &&
         strcmp(content, "changed on the server\n") == 0,
         "T4b: downloaded content matches the server's edit");

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.uploaded == 0 && sum.downloaded == 0 && sum.errors == 0,
         "T4c: sync after download is stable (post-download md5 patch works)");

    /* ---- T5: local delete propagates ---- */
    printf("\n--- T5: local delete ---\n");
    remove(note2_path);
    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.deleted == 1 && sum.errors == 0, "T5a: local delete propagates to the server");

    {
        int status = 0;
        raw_get(&cfg, remote_note2, content, sizeof(content), &status);
        check(status == 404, "T5b: deleted file is really gone on the server");
    }

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.deleted == 0 && sum.uploaded == 0 && sum.downloaded == 0 && sum.errors == 0,
         "T5c: sync after delete is stable");

    /* ---- T6: genuine conflict -- diverging changes on both sides ---- */
    printf("\n--- T6: conflict ---\n");
    write_file(note1_path, "local edit X\n");
    raw_put(&cfg, remote_note1, "remote edit Y\n");

    progress_reset(&pr);
    cloudsync_run_t(t, local_root, remote_root, 0, 0, &pr, &sum);
    check(sum.conflicts == 1 && sum.uploaded == 0 && sum.downloaded == 0,
         "T6a: diverging changes are reported as a conflict, not resolved");

    check(read_file(note1_path, content, sizeof(content)) &&
         strcmp(content, "local edit X\n") == 0,
         "T6b: local file left untouched by the conflict");

    {
        int status = 0;
        int ok = raw_get(&cfg, remote_note1, content, sizeof(content), &status);
        check(ok && strcmp(content, "remote edit Y\n") == 0,
             "T6c: remote file left untouched by the conflict");
    }

    /* ---- Cleanup ---- */
    webdav_transport_close(t);
    raw_delete(&cfg, remote_root);
    remove(baseline_path);
    remove(note1_path);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    http_cleanup();
    return g_fail ? 1 : 0;
}
