/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * AmiCloudSync -- OwnCloud-Sync-Commodity for AmigaOS 3.x (see the
 * project plan). Registers a CxBroker (commodity.c) so it shows up in
 * Exchange and responds to Enable/Disable/Appear/Disappear/Kill; a
 * HotKey toggles a GadTools window (gui.c) listing local<->remote
 * folder pairs (pairs.c) that can be synced manually or automatically
 * on a timer.device interval (timer.c) via the transport-neutral sync
 * kernel bound to the WebDAV backend (libcloudsync's cloudsync_run_t +
 * webdav_transport_open) -- no AmiCloud/api.php code anywhere here, see
 * webdav.h.
 *
 * v1 design choice (RAM-constrained 4 MB target, see the plan): syncs
 * run INLINE in this one process -- the window just shows "Synchronisiere
 * ..." for the duration, Exchange/HotKey input is only processed again
 * once a sync completes. A background child process (the pattern already
 * used by handler/cloudhandler for its worker) is a natural later
 * upgrade if inline blocking ever proves too disruptive in practice.
 *
 * All settings -- the OwnCloud profile (server/username/password),
 * verify_ssl, the autosync interval, AND the folder-pair list -- are
 * edited by the separate amicloudsync-prefs program now (see repo plan
 * "Drei getrennte Prefs-Programme"), never by this commodity itself:
 * this GUI (gui.c) only ever DISPLAYS the pair list (read-only) and
 * triggers "Jetzt syncen"/"Alle syncen"; it never writes
 * AmiCloudSync.cfg. Reads ENV: first, then ENVARC: (see below) -- so
 * amicloudsync-prefs's "Benutzen" (ENV: only) takes effect on this
 * commodity's very next start, autosync tick, without needing a
 * "Speichern". No third fallback: if neither exists yet, that's simply
 * the very first run -- cfg/pl come back at their empty/default state
 * (see pairs_load()'s own doc), same as amicloudsync-prefs shows an
 * empty form in that case. Nothing to fall back TO.
 */
#include "amicloud.h"
#include "config.h"
#include "http.h"
#include "sync.h"
#include "webdav.h"
#include "pairs.h"
#include "commodity.h"
#include "timer.h"
#include "gui.h"
#include "wbstartup.h"
#include "ui_hooks.h"
#include "progress.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <dos/dos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

long __stack = 65536;
const char amiga_ver[] = "$VER: AmiCloudSync " AC_VERSION " (27.09.2026)";

#define PREFS_ENV     "ENV:AmiCloudSync.cfg"
#define PREFS_ENVARC  "ENVARC:AmiCloudSync.cfg"
#define DEFAULT_HOTKEY "ctrl alt a"

/* CX_POPKEY is the conventional tooltype name real Amiga commodities use
 * for their hotkey, so this honours it rather than inventing a new one.
 * The actual WBStartup/icon.library dance lives in wbstartup.h/.c
 * (libcloudcore) -- factored out from here and amicloud-commander-mui/
 * amivcs-gui-mui, which each had their own copy of exactly this. */
static void configure_hotkey(int argc, char *argv[], char *hotkey, size_t hotkey_sz)
{
    strncpy(hotkey, DEFAULT_HOTKEY, hotkey_sz - 1);
    hotkey[hotkey_sz - 1] = '\0';

    if (argc > 0) {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
                strncpy(hotkey, argv[i + 1], hotkey_sz - 1);
                hotkey[hotkey_sz - 1] = '\0';
            }
        }
    } else {
        ac_wb_tooltype(argc, argv, "CX_POPKEY", hotkey, hotkey_sz);
    }
}

/* Live per-file progress in the pair window's status line while a sync
 * runs -- mirrors ui_hooks.c's own console_progress() (same throttling
 * via UiRateMeter, ~1 redraw/sec, always flush the last sample so a
 * transfer visibly reaches 100%), but calls gui_set_status() instead of
 * printf(). Without this, the window just shows a static "Synchronisiere
 * ..." for the whole (inline, blocking) transfer -- http_amiga.c already
 * calls ui_progress() once per chunk during every PUT/GET, this commodity
 * just never had a hook installed to receive it. GT_SetGadgetAttrs()
 * (inside gui_set_status()) redraws immediately, no event-loop pump
 * needed even though this whole process is blocked inside cloudsync_run_t()
 * for the duration. */
static UiRateMeter g_sync_rate;
static char        g_sync_last_name[64];

static int sync_progress(long done, long total, const char *name)
{
    char line[256], rate[24];
    int  redraw;

    /* Ctrl-C ("Break" on this process) stops the sync; the signal stays
     * set, so the main loop then ends the commodity as usual. */
    if (ui_break_pending()) return 0;
    if (!name) name = "";
    if (strcmp(name, g_sync_last_name) != 0 || done < g_sync_rate.last_bytes) {
        ui_rate_reset(&g_sync_rate);
        strncpy(g_sync_last_name, name, sizeof(g_sync_last_name) - 1);
        g_sync_last_name[sizeof(g_sync_last_name) - 1] = '\0';
    }

    redraw = ui_rate_sample(&g_sync_rate, done) || (total > 0 && done >= total);
    if (!redraw) return 1;

    ui_format_rate(rate, sizeof(rate), g_sync_rate.bps);
    if (total > 0) {
        long pct = total > 2000000L ? done / (total / 100) : (done * 100) / total;
        snprintf(line, sizeof(line), "%s: %ld%% (%s)", name, pct, rate);
    } else {
        snprintf(line, sizeof(line), "%s: %ldK gelesen (%s)", name, done / 1024, rate);
    }
    gui_set_status(line);
    return 1;
}

static void do_sync_pair(const AcConfig *cfg, SyncPair *p, char *status, size_t status_sz)
{
    SyncTransport *t;
    Progress    pr;
    SyncSummary sum;

    if (!p->enabled) {
        snprintf(status, status_sz, "%s: deaktiviert, uebersprungen", p->local);
        return;
    }

    t = webdav_transport_open(cfg);
    if (!t) { snprintf(status, status_sz, "%s: kein Speicher", p->local); return; }

    progress_reset(&pr);
    cloudsync_run_t(t, p->local, p->remote, 0, 0, &pr, &sum);
    webdav_transport_close(t);

    snprintf(status, status_sz, "%s: %d hoch, %d runter, %d Konflikt(e)%s",
            p->local, sum.uploaded, sum.downloaded, sum.conflicts,
            sum.errors ? ", Fehler!" : "");
}

static int profile_incomplete(const AcConfig *cfg)
{
    return !cfg->server[0] || !cfg->username[0] || !cfg->password[0];
}

/* This process loads AmiCloudSync.cfg exactly once at startup and
 * then runs for hours/days (Exchange-managed commodity) -- if
 * amicloudsync-prefs edits it (Benutzen/Speichern) while THIS is
 * already running, nothing here would ever notice without an explicit
 * re-read. Called before the pair window opens and before every
 * autosync tick, so a running commodity picks up edited settings
 * without needing to be killed and relaunched. Frees the old pl->items
 * first -- pairs_load()'s own pairs_init() only NULLs the pointer, it
 * doesn't free what it's about to replace.
 *
 * ENV: -> ENVARC:, no further fallback: if neither exists yet, that IS
 * the very first run -- cfg/pl simply come back empty (pairs_load()'s
 * own documented behaviour for a missing file), nothing to fall back
 * to. Picking ENVARC: as the path here in that case is arbitrary (both
 * are equally absent); it's never read from since it doesn't exist. */
static void reload_prefs(AcConfig *cfg, PairList *pl, char *prefs_path, size_t prefs_path_sz)
{
    AcConfig probe;

    pairs_free(pl);
    memset(cfg, 0, sizeof(*cfg));
    memset(&probe, 0, sizeof(probe));

    if (config_load(PREFS_ENV, &probe)) {
        strncpy(prefs_path, PREFS_ENV, prefs_path_sz - 1);
    } else {
        strncpy(prefs_path, PREFS_ENVARC, prefs_path_sz - 1);
    }
    prefs_path[prefs_path_sz - 1] = '\0';

    pairs_load(prefs_path, cfg, pl);

    /* Wire cfg->debug (e.g. "debug=T:AmiCloudSync.log" in AmiCloudSync.cfg)
     * to http_amiga.c's verbose HTTP log AND sync.c's ac_log_printf() error
     * detail (see sync.c's out->errors++ sites) -- same mechanism
     * handler/cloudhandler/src/worker.c already uses for its own
     * console-less background process. Without this, a failed sync (e.g.
     * "0 hoch, 0 runter, 0 Konflikt(e), Fehler!") gives no way to see
     * WHICH file or HTTP call actually failed: this commodity has no
     * console of its own and never calls ui_set_hooks(), so ui_notify()'s
     * default printf() goes nowhere when launched from Workbench/
     * WBStartup. Re-run on every reload_prefs() call, not just at
     * startup, so editing debug= in amicloudsync-prefs and clicking "Neu
     * laden" takes effect without a restart. */
    if (cfg->debug[0]) {
        http_log_bptr = 1;
        strncpy(http_log_path, cfg->debug, sizeof(http_log_path) - 1);
        http_log_path[sizeof(http_log_path) - 1] = '\0';
    } else {
        http_log_bptr = 0;
        http_log_path[0] = '\0';
    }
}

/* A Workbench-launched commodity has no console: without a requester a
 * startup failure would just make it vanish. IntuitionBase belongs to
 * gui.c and is still NULL this early, so it is opened only for the
 * requester. From a Shell the text also goes to stderr. */
static void startup_error(int from_shell, const char *text)
{
    struct EasyStruct es;
    ULONG args[1];

    if (from_shell) fprintf(stderr, "AmiCloudSync: %s\n", text);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37L);
    if (!IntuitionBase) return;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"AmiCloudSync";
    es.es_TextFormat   = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)"OK";
    args[0] = (ULONG)text;
    EasyRequestArgs(NULL, &es, NULL, args);
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = NULL;
}

int main(int argc, char *argv[])
{
    AcConfig cfg;
    PairList pl;
    char prefs_path[AC_MAX_PATH];
    char hotkey[64];
    int window_open, running, autosync_enabled;

    memset(&cfg, 0, sizeof(cfg));
    pairs_init(&pl); /* pl->items must be NULL before reload_prefs()'s first pairs_free() */
    configure_hotkey(argc, argv, hotkey, sizeof(hotkey));

    /* Route per-chunk transfer progress into the pair window's status
     * line (see sync_progress()'s own doc comment) instead of the
     * shared copy engine's default console output, which would go
     * nowhere for a Workbench-launched, console-less commodity anyway
     * -- same off-switch amicloud-commander-mui/-qt already flip for
     * the same reason (see progress.h's own doc comment on
     * ac_console_progress). notify/confirm/overwrite left NULL, so they
     * keep their current (console) defaults -- this commodity has no
     * use for confirm()/overwrite() (do_sync_pair() never asks), and
     * notify() failures are already surfaced via ac_log_printf() (see
     * sync.c) once debug= is set. */
    ac_console_progress = 0;
    {
        UiHooks h;
        memset(&h, 0, sizeof(h));
        h.progress = sync_progress;
        ui_set_hooks(&h);
    }

    /* ENV: first, then ENVARC: -- amicloudsync-prefs's "Benutzen" writes
     * only ENV: (see repo plan's USE/SAVE/CANCEL semantics), so it must
     * outrank ENVARC: here for that to take effect without a
     * "Speichern". */
    reload_prefs(&cfg, &pl, prefs_path, sizeof(prefs_path));

    if (!http_init()) {
        char msg[400];
        const char *why = http_last_error();
        snprintf(msg, sizeof(msg),
                 "Netzwerk-Initialisierung fehlgeschlagen%s%s\n\n"
                 "Benoetigt werden ein laufender TCP/IP-Stack\n"
                 "(bsdsocket.library) und AmiSSL 5.",
                 *why ? ":\n" : ".", why);
        startup_error(argc > 0, msg);
        pairs_free(&pl);
        return 1;
    }

    if (!ac_commodity_open("AMICLOUDSYNC", "AmiCloudSync",
                           "OwnCloud-Ordnerabgleich", hotkey)) {
        /* NULL = another instance is already running and has been told
         * to show its window -- quitting quietly is correct then. */
        const char *why = ac_commodity_error();
        if (why) startup_error(argc > 0, why);
        ac_commodity_close();
        http_cleanup();
        pairs_free(&pl);
        return why ? 1 : 0;
    }

    autosync_enabled = (pl.interval_minutes > 0) && ac_timer_open();
    if (autosync_enabled) ac_timer_start((long)pl.interval_minutes * 60);

    window_open = 0;
    running = 1;

    while (running) {
        ULONG sigmask = ac_commodity_sigmask() | SIGBREAKF_CTRL_C;
        ULONG sig;

        if (autosync_enabled) sigmask |= ac_timer_sigmask();
        if (window_open)      sigmask |= gui_sigmask();

        sig = Wait(sigmask);

        if (sig & SIGBREAKF_CTRL_C) running = 0;

        if (sig & ac_commodity_sigmask()) {
            CxAction act = ac_commodity_poll();
            switch (act) {
            case CX_ACTION_QUIT:
                running = 0;
                break;
            case CX_ACTION_TOGGLE_WINDOW:
                if (window_open) { gui_close(); window_open = 0; }
                else {
                    /* Pick up anything amicloudsync-prefs saved while
                     * this commodity sat running in the background --
                     * see reload_prefs()'s own comment. */
                    reload_prefs(&cfg, &pl, prefs_path, sizeof(prefs_path));
                    window_open = gui_open(&pl, "AmiCloudSync");
                }
                break;
            case CX_ACTION_SHOW_WINDOW:
                if (!window_open) {
                    reload_prefs(&cfg, &pl, prefs_path, sizeof(prefs_path));
                    window_open = gui_open(&pl, "AmiCloudSync");
                } else {
                    gui_show();
                }
                break;
            case CX_ACTION_HIDE_WINDOW:
                if (window_open) { gui_close(); window_open = 0; }
                break;
            default:
                break;
            }
        }

        if (autosync_enabled && (sig & ac_timer_sigmask())) {
            ac_timer_consume();
            /* Same reload as opening the window -- an autosync tick is
             * the other place "did amicloudsync-prefs save something
             * new while I was running" needs to be picked up. */
            reload_prefs(&cfg, &pl, prefs_path, sizeof(prefs_path));
            if (window_open) gui_refresh_list(&pl);
            if (profile_incomplete(&cfg)) {
                if (window_open) gui_set_status("Profil unvollstaendig (Server/Benutzer/Passwort) -- siehe AmiCloudSync.cfg");
            } else {
                int i;
                for (i = 0; i < pl.n; i++) {
                    char status[256];
                    do_sync_pair(&cfg, &pl.items[i], status, sizeof(status));
                    if (window_open) gui_set_status(status);
                }
            }
            ac_timer_start((long)pl.interval_minutes * 60);
        }

        if (window_open && (sig & gui_sigmask())) {
            int sel = -1;
            GuiAction act = gui_poll(&pl, &sel);
            switch (act) {
            case GUI_ACT_CLOSE:
                /* Nothing to persist here anymore -- this window is
                 * read-only (see repo plan part D); pair edits happen
                 * exclusively in amicloudsync-prefs now. */
                gui_close();
                window_open = 0;
                break;
            case GUI_ACT_SYNC_ONE:
                if (profile_incomplete(&cfg)) {
                    gui_set_status("Profil unvollstaendig (Server/Benutzer/Passwort) -- siehe AmiCloudSync.cfg");
                } else if (sel >= 0 && sel < pl.n) {
                    char status[256];
                    gui_set_status("Synchronisiere...");
                    do_sync_pair(&cfg, &pl.items[sel], status, sizeof(status));
                    gui_set_status(status);
                }
                break;
            case GUI_ACT_SYNC_ALL:
                if (profile_incomplete(&cfg)) {
                    gui_set_status("Profil unvollstaendig (Server/Benutzer/Passwort) -- siehe AmiCloudSync.cfg");
                } else {
                    int i;
                    for (i = 0; i < pl.n; i++) {
                        char status[256];
                        gui_set_status("Synchronisiere...");
                        do_sync_pair(&cfg, &pl.items[i], status, sizeof(status));
                        gui_set_status(status);
                    }
                }
                break;
            case GUI_ACT_REFRESH:
                /* Manual "I just saved in AmiCloudSyncPrefs, notice
                 * it NOW" -- reload_prefs() already runs automatically
                 * on window-open and before every autosync tick, but
                 * this window may have been open the whole time. */
                reload_prefs(&cfg, &pl, prefs_path, sizeof(prefs_path));
                gui_refresh_list(&pl);
                gui_set_status("Neu geladen.");
                break;
            default:
                break;
            }
        }
    }

    if (window_open) gui_close();
    if (autosync_enabled) ac_timer_close();
    ac_commodity_close();
    pairs_free(&pl);
    http_cleanup();
    return 0;
}
