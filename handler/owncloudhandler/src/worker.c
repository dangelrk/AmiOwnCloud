/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * worker.c -- amiowncloud-worker: a normal AmigaOS process (full vbcc
 * startup, libc, bsdsocket/AmiSSL) that does all HTTP/TLS work on behalf
 * of the OWNCLOUD: handler, which is exec.library-only and can never
 * touch the network stack itself (see handler.c's file header). Talks to
 * the handler over proto.h's WorkerMsg protocol through the well-known
 * port AC_WORKER_PORT ("AmiOwnCloud-Worker").
 *
 * Structurally identical to handler/cloudhandler/src/worker.c -- same
 * dispatch()/ensure_net_ready()/map_get_status()/map_put_status() shape,
 * same silent UI hooks, same gmt_offset/dst-aware date conversion via
 * libcloudcore's cloudtime.h. The only real difference is WHICH command
 * layer dispatch() calls: libcloudhttp's webdavcmd.c (WebDAV/RFC 4918
 * against a plain OwnCloud/Nextcloud server) instead of cmd.c (AmiCloud's
 * own api.php). See webdavcmd.h for why this is its own module rather
 * than reusing libcloudsync's file-based WebDAV sync transport.
 *
 * Links libcloudhttp (webdavcmd.c/http_amiga.c) + libcloudcore (base64.c/
 * md5.c/config.c/ui_hooks.c/cloudtime.c/progress.c) -- see
 * Makefile.amiga-cross. Deliberately excludes cmd.c and parson.c: this
 * worker never speaks AmiCloud's JSON api.php protocol, so it has no use
 * for either (see webdavcmd.h's file header) -- a smaller link than
 * amicloud-worker's.
 *
 * WREQ_SETCOMMENT/WREQ_SETPROTECT are answered as a no-op success (result
 * = 0) rather than reaching the server: plain WebDAV/ownCloud has no
 * standard equivalent for an Amiga filecomment or RWED protection bits.
 * Silently accepting these (instead of ERROR_ACTION_NOT_KNOWN) avoids
 * spurious failures from Workbench Copy/icon-handling and similar tools
 * that routinely try to propagate them; the values are simply not
 * persisted anywhere.
 */
#ifdef PLATFORM_AMIGA

#include <dos/dos.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "proto.h"
#include "amicloud.h"
#include "webdavcmd.h"
#include "charset.h"
#include "config.h"
#include "http.h"
#include "ui_hooks.h"
#include "cloudtime.h" /* ac_datestamp_to_epoch/ac_epoch_to_datestamp --
                        * libcloudcore, the ONE place this conversion
                        * lives (module-encapsulation rule). */

extern int ac_errors_to_stderr; /* http_amiga.c */

const char amiga_ver[] = "$VER: amiowncloud-worker " AC_VERSION " (" AC_DATE ")";

static AcConfig g_cfg;
static int      g_net_ready = 0;

/* Thin wrappers binding libcloudcore's ac_*_datestamp() to this worker's
 * g_cfg -- see handler/cloudhandler/src/worker.c's identical wrappers for
 * the full rationale (the Amiga system clock carries no timezone info at
 * all; only the worker ever loads a config with gmt_offset/dst). */
static LONG datestamp_to_epoch(const struct DateStamp *ds)
{
    return ac_datestamp_to_epoch(ds, g_cfg.gmt_offset, g_cfg.dst);
}

static void epoch_to_datestamp(LONG epoch, struct DateStamp *ds)
{
    ac_epoch_to_datestamp(epoch, g_cfg.gmt_offset, g_cfg.dst, ds);
}

/* Deferred network init -- see handler/cloudhandler/src/worker.c's
 * identical function for the full "boot-order trap" rationale
 * (CreatePort() happens before this is ever called, so a Mount at
 * Activate=1/early boot doesn't hang waiting for Roadshow). Callers MUST
 * check this before touching any webdavcmd_*() function. */
static int ensure_net_ready(void)
{
    if (g_net_ready) return 1;
    if (http_init()) {
        http_warmup(g_cfg.server, g_cfg.verify_ssl);
        g_net_ready = 1;
    }
    return g_net_ready;
}

/* ---- silent UI hooks -- see handler/cloudhandler/src/worker.c's twin. */
static void     w_notify(int level, const char *msg)   { (void)level; (void)msg; }
static int      w_confirm(const char *msg)              { (void)msg; return 1; }
static int      w_progress(long done, long total, const char *name)
{
    (void)done; (void)total; (void)name;
    /* "Break <worker>" (Ctrl-C) cancels the running transfer -- the
     * only way to stop a long one, the handler itself never can. */
    return !ui_break_pending();
}
static UiOverwrite w_overwrite(int is_dir, const char *name,
                               long src_size, const char *src_date, const char *src_md5,
                               long dst_size, const char *dst_date, const char *dst_md5)
{
    (void)is_dir; (void)name; (void)src_size; (void)src_date; (void)src_md5;
    (void)dst_size; (void)dst_date; (void)dst_md5;
    return UI_OW_OVERWRITE; /* never actually consulted by the webdavcmd_*() calls below */
}

/* A WREQ_PUT that failed would lose the data for good: Close() frees the
 * handle either way (actions.c act_end()). Keep a copy in
 * T:AmiOwnCloud-unsaved/<name> instead, overwriting an older one of the
 * same name. Best effort -- nothing else to do if even that fails. */
static void save_unsaved(const char *path, const UBYTE *buf, ULONG len)
{
    const char *name = strrchr(path, '/');
    char target[300];
    BPTR lock, fh;

    name = name ? name + 1 : path;
    if (!*name) return;
    lock = CreateDir((CONST_STRPTR)"T:AmiOwnCloud-unsaved");
    if (lock) UnLock(lock);
    snprintf(target, sizeof(target), "T:AmiOwnCloud-unsaved/%s", name);
    fh = Open((CONST_STRPTR)target, MODE_NEWFILE);
    if (!fh) return;
    if (len) Write(fh, (APTR)buf, (LONG)len);
    Close(fh);
}

/* ---- HTTP status -> DOS error mapping -- same table as amicloud-worker
 * (handler/cloudhandler/src/worker.c); webdavcmd_*() exposes the same
 * status_out/webdavcmd_last_status() split cmd_*() does. */
static LONG map_get_status(int status)
{
    switch (status) {
    case 404: return ERROR_OBJECT_NOT_FOUND;
    case 401: return ERROR_DISK_NOT_VALIDATED;
    case 403: return ERROR_WRITE_PROTECTED;
    case 0:   return ERROR_NO_DISK;       /* transport failure: server unreachable,
                                           * timed out or cancelled -- DOS offers
                                           * "Retry" for this one */
    default:  return ERROR_INVALID_LOCK;
    }
}

static LONG map_put_status(int status)
{
    switch (status) {
    case 404: return ERROR_OBJECT_NOT_FOUND;
    case 401: return ERROR_DISK_NOT_VALIDATED;
    case 403: return ERROR_WRITE_PROTECTED;
    case 423: return ERROR_OBJECT_IN_USE;      /* WebDAV lock held elsewhere */
    case 413: return ERROR_DISK_FULL;
    case 507: return ERROR_DISK_FULL;          /* quota exceeded */
    case 422: return ERROR_DISK_FULL;
    case 0:   return ERROR_NO_DISK;       /* transport failure, see map_get_status() */
    default:  return ERROR_INVALID_LOCK;
    }
}

static void dispatch(WorkerMsg *m)
{
    /* Phase 4: the handler speaks the local codeset, the server UTF-8
     * (see AmiCloud's worker.c). Paths go through the names remembered
     * from earlier listings. */
    static char upath[AC_MAX_PATH], upath2[AC_MAX_PATH];
    cs_path_to_utf8(m->path, upath, sizeof(upath));
    cs_path_to_utf8(m->path2, upath2, sizeof(upath2));

    switch (m->cmd) {
    case WREQ_PING:
        m->result = 0;
        break;

    case WREQ_LIST: {
        AcEntry *entries = NULL;
        int n = 0, i;
        WEntry *out;

        if (!ensure_net_ready()) { m->result = map_get_status(0); break; }
        if (!webdavcmd_list_raw(&g_cfg, upath, &entries, &n)) {
            m->result = map_get_status(webdavcmd_last_status());
            break;
        }
        m->entries = NULL;
        m->n_entries = 0;
        /* an empty listing too: the names remembered for it are stale */
        if (n == 0) cs_list_dir(m->path, upath, (const char *const *)0, 0, (char *)0, 0, 0);
        if (n > 0) {
            out = (WEntry *)AllocVec((ULONG)n * sizeof(WEntry), MEMF_PUBLIC | MEMF_CLEAR);
            if (!out) {
                webdavcmd_entries_free(entries);
                m->result = ERROR_NO_FREE_STORE;
                break;
            }
            /* names; those that would be one file here (case, umlauts,
             * lossy) get "_2" names that still lead to their own file */
            {
                const char **un = (const char **)AllocVec((ULONG)n * sizeof(char *), MEMF_PUBLIC);
                if (un) {
                    for (i = 0; i < n; i++) un[i] = entries[i].name;
                    cs_list_dir(m->path, upath, un, n, out[0].name, sizeof(WEntry),
                                sizeof(out[0].name));
                    FreeVec(un);
                } else {
                    for (i = 0; i < n; i++)
                        cs_list_name(m->path, upath, entries[i].name, out[i].name,
                                     sizeof(out[i].name));
                }
            }
            for (i = 0; i < n; i++) {
                out[i].is_dir = entries[i].is_dir;
                out[i].size = entries[i].size;
                epoch_to_datestamp(entries[i].mtime, &out[i].mtime);
                out[i].protection = entries[i].protection; /* always 0 -- see webdavcmd.h */
                strncpy(out[i].comment, entries[i].comment, sizeof(out[i].comment) - 1); /* always "" */
                strncpy(out[i].md5, entries[i].md5, sizeof(out[i].md5) - 1); /* always "" */
            }
            /* a server name longer than a FileInfoBlock holds (106) is
             * left out instead of shown shortened -- the short name would
             * lead nowhere, or to a different file */
            {
                int j = 0;
                for (i = 0; i < n; i++) {
                    char lname[512];
                    cs_from_utf8(entries[i].name, lname, sizeof(lname));
                    if (strlen(lname) > sizeof(out[0].name) - 1) continue;
                    if (j != i) out[j] = out[i];
                    j++;
                }
                n = j;
            }
            m->entries = out;
            m->n_entries = n;
        }
        webdavcmd_entries_free(entries);
        m->server_now = (LONG)time(NULL);
        m->result = 0;
        break;
    }

    case WREQ_GET: {
        char *buf = NULL;
        long len = 0;
        int status = 0;

        if (!ensure_net_ready()) { m->result = map_get_status(0); break; }
        /* webdavcmd_get_buf() returns 0 on success, non-zero on failure --
         * the opposite convention from webdavcmd_list_raw() above, same
         * asymmetry cmd_get_buf()/cmd_list_raw() have (see cmd.h). */
        if (webdavcmd_get_buf(&g_cfg, upath, &buf, &len, &status) != 0) {
            m->result = map_get_status(status);
            break;
        }
        m->buf = (UBYTE *)buf;
        m->buflen = (ULONG)len;
        m->result = 0;
        break;
    }

    case WREQ_READ: {
        char *buf = NULL;
        long len = 0;
        int status = 0;

        if (!ensure_net_ready()) { m->result = map_get_status(0); break; }
        if (webdavcmd_get_range(&g_cfg, upath, (long)m->offset, (long)m->buflen, &buf, &len, &status) != 0) {
            m->result = map_get_status(status);
            m->buf = NULL;
            m->buflen = 0;
            break;
        }
        m->buf = (UBYTE *)buf;
        m->buflen = (ULONG)len;
        m->result = 0;
        break;
    }

    case WREQ_PUT: {
        int status = 0;

        if (!ensure_net_ready()) {
            m->result = map_put_status(0);
            save_unsaved(m->path, m->buf, m->buflen);
            break;
        }
        if (webdavcmd_put_buf(&g_cfg, upath, (const char *)m->buf, (long)m->buflen, &status) != 0) {
            m->result = map_put_status(status);
            save_unsaved(m->path, m->buf, m->buflen);
            break;
        }
        m->result = 0;
        break;
    }

    case WREQ_DEL:
        if (!ensure_net_ready()) { m->result = map_put_status(0); break; }
        m->result = webdavcmd_del(&g_cfg, upath) ? map_put_status(webdavcmd_last_status()) : 0;
        break;

    case WREQ_MKDIR:
        if (!ensure_net_ready()) { m->result = map_put_status(0); break; }
        m->result = webdavcmd_mkdir_quiet(&g_cfg, upath) ? map_put_status(webdavcmd_last_status()) : 0;
        break;

    case WREQ_RENAME:
        if (!ensure_net_ready()) { m->result = map_put_status(0); break; }
        m->result = webdavcmd_rename(&g_cfg, upath, upath2) ? map_put_status(webdavcmd_last_status()) : 0;
        break;

    case WREQ_INFO: {
        AcInfo info;

        if (!ensure_net_ready()) { m->result = map_get_status(0); break; }
        if (webdavcmd_info_raw(&g_cfg, upath, &info) != 0) {
            m->result = map_get_status(webdavcmd_last_status());
            break;
        }
        epoch_to_datestamp(info.mtime, &m->mtime);
        m->protection = info.protection; /* always 0 -- see webdavcmd.h */
        strncpy(m->comment, info.comment, sizeof(m->comment) - 1); /* always "" */
        m->result = 0;
        break;
    }

    case WREQ_SETDATE:
        if (!ensure_net_ready()) { m->result = map_put_status(0); break; }
        m->result = webdavcmd_setdate_mtime(&g_cfg, upath, datestamp_to_epoch(&m->mtime))
                    ? map_put_status(webdavcmd_last_status()) : 0;
        break;

    case WREQ_SETCOMMENT:
        /* No WebDAV/ownCloud equivalent -- silently accepted, never sent
         * to the server (see this file's header comment). */
        m->result = 0;
        break;

    case WREQ_SETPROTECT:
        /* Same as WREQ_SETCOMMENT above. */
        m->result = 0;
        break;

    default:
        m->result = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}

int main(int argc, char **argv)
{
    struct MsgPort *port;
    int running = 1;

    (void)argc; (void)argv;

    config_load("ENV:amiowncloud.cfg", &g_cfg);
    cs_open(g_cfg.charset);   /* Phase 4: codesets.library, else ISO-8859-1 */

    {
        UiHooks h;
        h.notify    = w_notify;
        h.confirm   = w_confirm;
        h.progress  = w_progress;
        h.overwrite = w_overwrite;
        ui_set_hooks(&h);
    }
    ac_errors_to_stderr = 0;
    if (g_cfg.debug[0]) {
        http_log_bptr = 1;
        strncpy(http_log_path, g_cfg.debug, sizeof(http_log_path) - 1);
        http_log_path[sizeof(http_log_path) - 1] = '\0';
    }

    port = CreateMsgPort();
    if (!port) return RETURN_FAIL;
    port->mp_Node.ln_Name = (char *)AC_WORKER_PORT;
    port->mp_Node.ln_Pri  = 0;
    AddPort(port);

    while (running) {
        struct Message *raw;
        WorkerMsg *m;

        WaitPort(port);
        while ((raw = GetMsg(port)) != NULL) {
            m = (WorkerMsg *)raw;
            if (m->cmd == WREQ_QUIT) {
                m->result = 0;
                ReplyMsg((struct Message *)m);
                running = 0;
                continue;
            }
            dispatch(m);
            /* Every reply carries the clock and the cache lifetime, the
             * handler has neither (see proto.h). */
            m->server_now = (LONG)time(NULL);
            m->cache_ttl  = g_cfg.cache_ttl;
            /* A Break only cancels the request it hit, not every later one. */
            ui_break_clear();
            ReplyMsg((struct Message *)m);
        }
    }

    RemPort(port);
    DeleteMsgPort(port);
    if (g_net_ready) http_cleanup();
    return RETURN_OK;
}

#endif /* PLATFORM_AMIGA */
