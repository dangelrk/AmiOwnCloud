/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * owncloud-prefs -- Preferences editor for owncloud.cfg (owncloud-worker,
 * the backend behind the OWNCLOUD: DOS handler, handler/owncloudhandler
 * -- see config-examples/README.md's table). GadTools window built on
 * core/common's prefsgui.h toolbox (see repo plan "Drei getrennte
 * Prefs-Programme"), same field set as amicloud-prefs (AcConfig is
 * shared): own program, no dependency on amicloud-prefs, amivcs-prefs
 * or amicloudsync-prefs.
 *
 * owncloud.cfg talks WebDAV to a plain ownCloud/Nextcloud server, not
 * AmiCloud's api.php -- "server" here is either a bare host or a full
 * "/remote.php/dav/files/USER" collection URL (see webdavcmd.h), and
 * credentials are never shared with amicloud.cfg/AmiCloudSync.cfg (own
 * profile, config-examples/README.md).
 *
 * Classic Workbench-Prefs USE/SAVE/CANCEL semantics (bindend, see the
 * plan): Benutzen writes only ENV:owncloud.cfg (live now, gone after a
 * reboot); Speichern writes ENVARC:owncloud.cfg AND ENV:owncloud.cfg
 * (live now, and permanent); Abbrechen writes nothing. owncloud-worker
 * itself only ever reads ENV:owncloud.cfg (worker.c) -- "Benutzen"
 * takes effect on the worker's next spawn (e.g. after a fresh Mount);
 * an already-running worker needs killing and restarting to pick up
 * either.
 */
#include "amicloud.h"
#include "config.h"
#include "prefsgui.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

long __stack = 65536;
const char amiga_ver[] = "$VER: OwnCloudPrefs " AC_VERSION " (27.09.2026)";

#define GID_SERVER    1
#define GID_USER      2
#define GID_PASS      3
#define GID_BASEPATH  4
#define GID_VERIFYSSL 5
#define GID_DEBUG     6
#define GID_GMTOFFSET 7
#define GID_DST       8
#define GID_STATUS    9
#define GID_USE       10
#define GID_SAVE      11
#define GID_CANCEL    12

#define PREFS_ENV    "ENV:owncloud.cfg"
#define PREFS_ENVARC "ENVARC:owncloud.cfg"

int main(int argc, char *argv[])
{
    AcConfig cfg;
    struct Gadget *glist, *gad;
    struct Gadget *g_server, *g_user, *g_pass, *g_basepath;
    struct Gadget *g_verifyssl, *g_debug, *g_gmtoffset, *g_dst;
    struct Window *win;
    PrefsLayout lay;
    WORD winw, winh, topborder;
    int running;
    const char *notice;

    memset(&cfg, 0, sizeof(cfg));
    /* ENV: first -- that's the CURRENT/live copy (see the repo's
     * USE/SAVE/CANCEL semantics: "Benutzen" only ever writes ENV:).
     * ENVARC: is the durable fallback for when ENV: hasn't been
     * (re-)created yet this session (e.g. right after a reboot). NEVER
     * config_default_path()'s own fallback -- see amicloud-prefs's
     * identical comment for why. */
    if (!config_load(PREFS_ENV, &cfg)) config_load(PREFS_ENVARC, &cfg);
    notice = prefsgui_files_differ(PREFS_ENV, PREFS_ENVARC)
           ? "ENV:/ENVARC: unterscheiden sich (angezeigt: ENV:)"
           : "";

    if (!prefsgui_screen_open()) return 1;

    topborder = prefsgui_topborder();
    winw = 460;
    winh = topborder + 8 + 9 * 26 + 26 + 8;

    glist = NULL;
    gad = CreateContext(&glist);
    if (!gad) { prefsgui_screen_close(); return 1; }

    prefsgui_layout_init(&lay, gad, topborder + 8, 180, winw - 180 - 8, 20, 6);

    g_server = prefsgui_field_string(&lay, "Server", GID_SERVER, cfg.server, sizeof(cfg.server) - 1);
    if (!g_server) goto fail;

    g_user = prefsgui_field_string(&lay, "Benutzer", GID_USER, cfg.username, sizeof(cfg.username) - 1);
    if (!g_user) goto fail;

    g_pass = prefsgui_field_string(&lay, "Passwort", GID_PASS, cfg.password, sizeof(cfg.password) - 1);
    if (!g_pass) goto fail;

    g_basepath = prefsgui_field_string(&lay, "Basis-Pfad", GID_BASEPATH, cfg.basepath, sizeof(cfg.basepath) - 1);
    if (!g_basepath) goto fail;

    g_verifyssl = prefsgui_field_checkbox(&lay, "Zertifikat pruefen", GID_VERIFYSSL, cfg.verify_ssl);
    if (!g_verifyssl) goto fail;

    g_debug = prefsgui_field_string(&lay, "Debug-Ziel", GID_DEBUG, cfg.debug, sizeof(cfg.debug) - 1);
    if (!g_debug) goto fail;

    {
        WORD saved_w = lay.field_w;
        lay.field_w = 80;
        g_gmtoffset = prefsgui_field_integer(&lay, "GMT-Offset (Min)", GID_GMTOFFSET, cfg.gmt_offset);
        lay.field_w = saved_w;
    }
    if (!g_gmtoffset) goto fail;

    g_dst = prefsgui_field_checkbox(&lay, "Sommerzeit", GID_DST, cfg.dst);
    if (!g_dst) goto fail;

    gad = prefsgui_status_line(&lay, 8, winw - 16, GID_STATUS, notice);
    if (!gad) goto fail;

    gad = prefsgui_use_save_cancel_row(&lay, 8, winw - 16, GID_USE, GID_SAVE, GID_CANCEL);
    if (!gad) goto fail;

    win = OpenWindowTags(NULL,
        WA_Left,      60,
        WA_Top,       40,
        WA_Width,     winw,
        WA_Height,    winh,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,   (ULONG)glist,
        WA_Title,     (ULONG)"OwnCloud-Einstellungen",
        WA_PubScreen, (ULONG)prefsgui_screen(),
        TAG_DONE);
    if (!win) goto fail;

    GT_RefreshWindow(win, NULL);

    running = 1;
    while (running) {
        UWORD act = prefsgui_wait_action(win, NULL);

        if (act == GID_VERIFYSSL) {
            /* Tracked app-side, not read back via GT_GetGadgetAttrs --
             * see prefsgui_set_checkbox()'s doc comment in prefsgui.h. */
            cfg.verify_ssl = !cfg.verify_ssl;
            prefsgui_set_checkbox(win, g_verifyssl, cfg.verify_ssl);
        } else if (act == GID_DST) {
            cfg.dst = !cfg.dst;
            prefsgui_set_checkbox(win, g_dst, cfg.dst);
        } else if (act == GID_USE || act == GID_SAVE) {
            prefsgui_get_string(g_server,   cfg.server,   sizeof(cfg.server));
            prefsgui_get_string(g_user,     cfg.username, sizeof(cfg.username));
            prefsgui_get_string(g_pass,     cfg.password, sizeof(cfg.password));
            prefsgui_get_string(g_basepath, cfg.basepath, sizeof(cfg.basepath));
            prefsgui_get_string(g_debug,    cfg.debug,    sizeof(cfg.debug));
            cfg.gmt_offset = (int)prefsgui_get_integer(win, g_gmtoffset);

            config_save(PREFS_ENV, &cfg, CONFIG_SAVE_ALL);
            if (act == GID_SAVE) config_save(PREFS_ENVARC, &cfg, CONFIG_SAVE_ALL);
            running = 0;
        } else if (act == GID_CANCEL || act == PREFSGUI_ACT_CLOSE) {
            running = 0;
        }
    }

    CloseWindow(win);
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 0;

fail:
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 1;
}
