/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * amiowncloud-sync -- one-shot CLI front-end for libcloudsync's cloudsync_run_ex()
 * bound to the WebDAV transport (libcloudsync/webdav.h). Exists purely to
 * verify Teil A of the OwnCloud-Sync-Commodity plan on a PC/Mac, against
 * a real OwnCloud/Nextcloud server, before any Amiga GUI code is written.
 *
 * Deliberately separate from apps/cloudsync (which stays on the
 * AmiCloud/api.php transport, completely unchanged) -- see repo memory:
 * AmiCloud and OwnCloud must not be mixed. No PHP anywhere in this tool.
 *
 * Same config-loading convention as apps/cloudsync's main.c
 * (server/user/pass from -s/-u/-p or a config file). Here, cfg.server is
 * the WebDAV collection's own base URL, e.g.
 * "https://cloud.example.com/remote.php/dav/files/USER" -- not an
 * api.php endpoint (see webdav.h).
 */
#include "amicloud.h"
#include "config.h"
#include "http.h"
#include "sync.h"
#include "localfs.h"
#include "charset.h"
#include "webdav.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef PLATFORM_AMIGA
long __stack = 65536;
const char amiga_ver[] = "$VER: amiowncloud-sync " AC_VERSION " (" AC_DATE ")";
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
        "amiowncloud-sync v" AC_VERSION " -- one-shot local<->WebDAV folder sync\n"
        "Usage: %s [options] <local-dir> <remote-dir>\n"
        "\n"
        "Options:\n"
        "  -s <url>     Server -- either just the main host (e.g.\n"
        "               https://cloud.example.com, the collection URL is\n"
        "               derived from server+user automatically) or a full\n"
        "               WebDAV collection base URL, e.g.\n"
        "               https://host/remote.php/dav/files/USER\n"
        "  -u <user>    Username\n"
        "  -p <pass>    Password (Nextcloud: an app password)\n"
        "  -c <file>    Config file (default: ~/.amiowncloud-syncrc or ENV:amiowncloud-sync.cfg)\n"
        "  -k           Disable TLS certificate verification (development only)\n"
        "  -n           Dry run -- show what would happen, change nothing\n",
        prog);
    exit(1);
}

int main(int argc, char **argv)
{
    AcConfig cfg;
    const char *opt_server = NULL, *opt_user = NULL, *opt_pass = NULL, *opt_cfgfile = NULL;
    int opt_noverify = 0, opt_dryrun = 0;
    int i;
    const char *local_root, *remote_root;
    Progress      pr;
    SyncSummary   sum;
    char        remote_utf8[AC_MAX_PATH];
    SyncTransport *t;

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
        else if (strcmp(argv[i], "-n") == 0)               { opt_dryrun   = 1; }
        else if (strcmp(argv[i], "--help") == 0)           { usage(argv[0]); }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); usage(argv[0]); }
    }

    if (opt_cfgfile) {
        config_load(opt_cfgfile, &cfg);
    } else {
        char primary[AC_MAX_PATH], fallback[AC_MAX_PATH];
        config_default_path("amiowncloud-sync", primary, sizeof(primary), fallback, sizeof(fallback));
        if (!config_load(primary, &cfg) && fallback[0]) config_load(fallback, &cfg);
    }
    config_merge(&cfg, opt_server, opt_user, opt_pass, NULL);
    if (opt_noverify) cfg.verify_ssl = 0;

    if (i + 1 >= argc) { fprintf(stderr, "amiowncloud-sync: <local-dir> and <remote-dir> required\n"); usage(argv[0]); }
    local_root  = argv[i++];
    remote_root = argv[i];

    if (!cfg.server[0])   { fprintf(stderr, "amiowncloud-sync: WebDAV base URL required (-s or config file)\n"); usage(argv[0]); }
    if (!cfg.username[0]) { fprintf(stderr, "amiowncloud-sync: username required (-u or config file)\n");        usage(argv[0]); }
    if (!cfg.password[0]) { fprintf(stderr, "amiowncloud-sync: password required (-p or config file)\n");        usage(argv[0]); }

    {
        size_t len = strlen(cfg.server);
        if (len > 0 && cfg.server[len-1] == '/') cfg.server[len-1] = '\0';
    }

    if (!http_init()) {
        fprintf(stderr, "amiowncloud-sync: network initialisation failed\n");
        return 1;
    }

    /* S16: local file dates in local time, like the shell and the
     * handler (gmt_offset/dst from the config; no-op on the PC) */
    localfs_set_tz(cfg.gmt_offset, cfg.dst);

    /* Phase 4: local file names in the system codeset (codesets.library);
     * the remote folder from the command line is converted to UTF-8 */
    if (!cs_open(cfg.charset))
        fprintf(stderr, "amiowncloud-sync: codesets.library (ab 6) nicht gefunden -- "
                "Dateinamen werden als ISO-8859-1 behandelt\n");
    cs_to_utf8(remote_root, remote_utf8, sizeof(remote_utf8));

    t = webdav_transport_open(&cfg);
    if (!t) {
        fprintf(stderr, "amiowncloud-sync: out of memory\n");
        http_cleanup();
        return 1;
    }

    progress_reset(&pr);
    {
        SyncOptions o;
        sync_options_init(&o);
        o.dry_run = opt_dryrun;
        o.verbose = 1;
        cloudsync_run_ex(t, local_root, remote_utf8, &o, &pr, &sum);
    }

    webdav_transport_close(t);
    http_cleanup();
    return sum.errors ? 1 : 0;
}
