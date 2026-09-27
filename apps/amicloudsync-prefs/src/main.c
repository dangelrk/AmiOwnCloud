/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * amicloudsync-prefs -- Preferences editor for AmiCloudSync.cfg (the
 * AmiCloudSync commodity, apps/amicloud-sync-cx). GadTools
 * window built on core/common's prefsgui.h toolbox (see repo plan
 * "Drei getrennte Prefs-Programme"); own program, no dependency on
 * amicloud-prefs or amivcs-prefs.
 *
 * Owns BOTH halves of AmiCloudSync.cfg: the OwnCloud profile
 * (server/user/pass/verify_ssl) AND this commodity's own interval=/
 * pair= lines (apps/amicloud-sync-cx/src/pairs.h) -- the
 * folder-pair add/edit/remove UI that used to live in the commodity's
 * own gui.c has moved here; the commodity itself now only shows a
 * read-only list and syncs (see repo plan part D). pairs.{c,h} stays
 * the one and only source for the PairList/SyncPair format -- this app
 * compiles that same file, never a copy.
 *
 * "Nur Hauptserver": Server here is just the bare host, e.g.
 * "https://cloud.example.com" -- libcloudsync/src/webdav.c derives the
 * actual WebDAV collection URL from server+user at sync time. A full
 * legacy collection URL (containing "/remote.php/") still works too.
 *
 * Classic Workbench-Prefs USE/SAVE/CANCEL semantics (bindend, see the
 * plan): Benutzen writes only ENV:AmiCloudSync.cfg (live now, gone
 * after a reboot); Speichern writes ENVARC:AmiCloudSync.cfg AND
 * ENV:AmiCloudSync.cfg (live now, and permanent); Abbrechen writes
 * nothing. The commodity itself now reads ENV: first, then ENVARC: (see
 * its main.c) -- so "Benutzen" takes effect on its very next autosync
 * tick or "Alle syncen" click without a restart. No third fallback: if
 * neither exists, that's simply the very first run -- every field
 * starts empty.
 */
#include "amicloud.h"
#include "config.h"
#include "prefsgui.h"
#include "pairs.h"

#include <exec/types.h>
#include <exec/lists.h>
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
const char amiga_ver[] = "$VER: AmiCloudSyncPrefs " AC_VERSION " (14.07.2026)";

#define PREFS_ENV     "ENV:AmiCloudSync.cfg"
#define PREFS_ENVARC  "ENVARC:AmiCloudSync.cfg"

/* ------------------------------------------------------------------ */
/* NewList()/AddTail()/RemHead() are amiga.lib (alib) helpers, not real
 * exec.library calls -- hand-rolled here rather than linking amiga.lib
 * for three trivial pointer-chases, same code (and rationale) as
 * apps/amicloud-sync-cx/src/gui.c. */
static void ac_list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void ac_list_addtail(struct List *l, struct Node *n)
{
    n->ln_Succ = (struct Node *)&l->lh_Tail;
    n->ln_Pred = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred = n;
}

static struct Node *ac_list_remhead(struct List *l)
{
    struct Node *n = l->lh_Head;
    if (n->ln_Succ == NULL) return NULL; /* empty */
    l->lh_Head = n->ln_Succ;
    l->lh_Head->ln_Pred = (struct Node *)&l->lh_Head;
    return n;
}

static struct List g_lv_list;
static int          g_lv_built;

static void free_lv_list(void)
{
    struct Node *n;
    if (!g_lv_built) return;
    while ((n = ac_list_remhead(&g_lv_list)) != NULL) {
        free(n->ln_Name);
        free(n);
    }
    g_lv_built = 0;
}

static void build_lv_list(const PairList *pl)
{
    int i;
    ac_list_init(&g_lv_list);
    g_lv_built = 1;
    for (i = 0; i < pl->n; i++) {
        const SyncPair *p = &pl->items[i];
        struct Node *n;
        char *label;
        size_t need;

        n = (struct Node *)malloc(sizeof(struct Node));
        if (!n) continue;
        need = strlen(p->local) + strlen(p->remote) + 8;
        label = (char *)malloc(need);
        if (!label) { free(n); continue; }
        snprintf(label, need, "%c %s -> %s", p->enabled ? '*' : ' ', p->local, p->remote);

        memset(n, 0, sizeof(*n));
        n->ln_Name = label;
        ac_list_addtail(&g_lv_list, n);
    }
}

static void refresh_listview(struct Window *win, struct Gadget *lv, const PairList *pl)
{
    /* Detach the old list before touching it -- the documented GadTools
     * idiom for safely swapping a listview's backing list at runtime. */
    GT_SetGadgetAttrs(lv, win, NULL, GTLV_Labels, (ULONG)~0, TAG_DONE);
    free_lv_list();
    build_lv_list(pl);
    GT_SetGadgetAttrs(lv, win, NULL, GTLV_Labels, (ULONG)&g_lv_list, TAG_DONE);
}

/* ------------------------------------------------------------------ */
/* Add/Edit modal dialog for a single folder pair -- own small window,   */
/* own blocking prefsgui_wait_action() loop, reusing the main window's   */
/* screen/VisualInfo via prefsgui's field helpers (only ever callable    */
/* while the main window is open). Modelled on the dialog that used to   */
/* live in amicloud-sync-cx/src/gui.c's run_pair_dialog().                */
/* ------------------------------------------------------------------ */

#define DGID_LOCAL  1
#define DGID_REMOTE 2
#define DGID_ENABLE 3
#define DGID_OK     4
#define DGID_CANCEL 5

static int run_pair_dialog(const SyncPair *existing, SyncPair *out)
{
    struct Gadget *glist, *gad;
    struct Gadget *g_local, *g_remote, *g_enable;
    struct Window *win;
    PrefsLayout lay;
    WORD winw, winh, topborder;
    int result = 0;
    /* Tracked app-side, not read back via GT_GetGadgetAttrs at OK time --
     * see prefsgui_set_checkbox()'s doc comment in prefsgui.h. */
    int enable_state = existing ? existing->enabled : 1;

    topborder = prefsgui_topborder();
    winw = 360;
    winh = topborder + 8 + 3 * 26 + 26 + 8;

    glist = NULL;
    gad = CreateContext(&glist);
    if (!gad) return 0;

    prefsgui_layout_init(&lay, gad, topborder + 8, 112, winw - 112 - 8, 20, 6);

    g_local = prefsgui_field_string(&lay, "Lokaler Pfad", DGID_LOCAL,
                                    existing ? existing->local : "", AC_MAX_PATH - 1);
    if (!g_local) { FreeGadgets(glist); return 0; }

    g_remote = prefsgui_field_string(&lay, "Remote-Pfad", DGID_REMOTE,
                                     existing ? existing->remote : "/", AC_MAX_PATH - 1);
    if (!g_remote) { FreeGadgets(glist); return 0; }

    g_enable = prefsgui_field_checkbox(&lay, "Aktiv", DGID_ENABLE,
                                       existing ? existing->enabled : 1);
    if (!g_enable) { FreeGadgets(glist); return 0; }

    {
        /* Two buttons (OK/Abbrechen) -- not prefsgui's fixed three-button
         * Benutzen/Speichern/Abbrechen row, so built manually here, same
         * shape as gui.c's original run_pair_dialog(). */
        struct NewGadget ng;
        WORD bw = (winw - 24) / 2;

        memset(&ng, 0, sizeof(ng));
        ng.ng_TextAttr   = prefsgui_font();
        ng.ng_VisualInfo = prefsgui_vi();
        ng.ng_TopEdge    = lay.y;
        ng.ng_Height     = lay.row_h;
        ng.ng_Width      = bw;
        ng.ng_Flags      = PLACETEXT_IN;

        ng.ng_LeftEdge   = 8;
        ng.ng_GadgetText = (CONST_STRPTR)"OK";
        ng.ng_GadgetID   = DGID_OK;
        gad = CreateGadget(BUTTON_KIND, lay.context, &ng, TAG_DONE);
        if (!gad) { FreeGadgets(glist); return 0; }
        lay.context = gad;

        ng.ng_LeftEdge   = 8 + bw + 8;
        ng.ng_GadgetText = (CONST_STRPTR)"Abbrechen";
        ng.ng_GadgetID   = DGID_CANCEL;
        gad = CreateGadget(BUTTON_KIND, lay.context, &ng, TAG_DONE);
        if (!gad) { FreeGadgets(glist); return 0; }
        lay.context = gad;
    }

    win = OpenWindowTags(NULL,
        WA_Left,      80,
        WA_Top,       80,
        WA_Width,     winw,
        WA_Height,    winh,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,   (ULONG)glist,
        WA_Title,     (ULONG)(existing ? "Ordnerpaar bearbeiten" : "Ordnerpaar hinzuf\xfcgen"),
        WA_PubScreen, (ULONG)prefsgui_screen(),
        TAG_DONE);
    if (!win) { FreeGadgets(glist); return 0; }

    GT_RefreshWindow(win, NULL);

    for (;;) {
        UWORD act = prefsgui_wait_action(win, NULL);
        if (act == DGID_ENABLE) {
            enable_state = !enable_state;
            prefsgui_set_checkbox(win, g_enable, enable_state);
            continue;
        }
        if (act == DGID_OK)                          { result = 1; break; }
        if (act == DGID_CANCEL || act == PREFSGUI_ACT_CLOSE) { result = 0; break; }
    }

    if (result) {
        prefsgui_get_string(g_local,  out->local,  sizeof(out->local));
        prefsgui_get_string(g_remote, out->remote, sizeof(out->remote));
        out->enabled = enable_state;
        if (!out->local[0] || !out->remote[0]) result = 0; /* refuse an empty pair silently */
    }

    CloseWindow(win);
    FreeGadgets(glist);
    return result;
}

/* ------------------------------------------------------------------ */
/* Main window                                                           */
/* ------------------------------------------------------------------ */

#define GID_SERVER    1
#define GID_USER      2
#define GID_PASS      3
#define GID_VERIFYSSL 4
#define GID_INTERVAL  5
#define GID_STATUS    6
#define GID_LISTVIEW  7
#define GID_ADD       8
#define GID_EDIT      9
#define GID_REMOVE    10
#define GID_USE       11
#define GID_SAVE      12
#define GID_CANCEL    13

int main(int argc, char *argv[])
{
    AcConfig cfg;
    PairList pl;
    char prefs_path[AC_MAX_PATH];
    struct Gadget *glist, *gad;
    struct Gadget *g_server, *g_user, *g_pass, *g_verifyssl, *g_interval, *g_listview;
    struct Window *win;
    PrefsLayout lay;
    WORD winw, winh, topborder;
    int running, selected;
    const char *notice;

    memset(&cfg, 0, sizeof(cfg));

    /* Same ENV:->ENVARC: cascade as the commodity's own main.c (repo
     * plan part D) -- shows whichever copy is currently "live", same
     * file config_load() would pick for a running commodity right now.
     * No third fallback: if neither exists, that's simply the very
     * first run -- every field starts empty, nothing to fall back to. */
    {
        AcConfig probe;
        memset(&probe, 0, sizeof(probe));
        if (config_load(PREFS_ENV, &probe)) {
            strncpy(prefs_path, PREFS_ENV, sizeof(prefs_path) - 1);
        } else {
            strncpy(prefs_path, PREFS_ENVARC, sizeof(prefs_path) - 1);
        }
        prefs_path[sizeof(prefs_path) - 1] = '\0';
    }
    pairs_load(prefs_path, &cfg, &pl);
    notice = prefsgui_files_differ(PREFS_ENV, PREFS_ENVARC)
           ? "ENV:/ENVARC: unterscheiden sich (angezeigt: ENV:)"
           : "";

    if (!prefsgui_screen_open()) { pairs_free(&pl); return 1; }

    topborder = prefsgui_topborder();
    winw = 460;
    winh = topborder + 8
         + 5 * 26   /* profile fields: server/user/pass/verifyssl/interval */
         + 26         /* status line */
         + 100 + 8   /* folder-pair listview */
         + 26         /* Hinzufuegen/Bearbeiten/Entfernen row */
         + 26         /* Benutzen/Speichern/Abbrechen row */
         + 8;         /* bottom margin */

    glist = NULL;
    gad = CreateContext(&glist);
    if (!gad) goto fail;

    prefsgui_layout_init(&lay, gad, topborder + 8, 180, winw - 180 - 8, 20, 6);

    g_server = prefsgui_field_string(&lay, "Server (Hauptserver)", GID_SERVER,
                                     cfg.server, sizeof(cfg.server) - 1);
    if (!g_server) goto fail;

    g_user = prefsgui_field_string(&lay, "Benutzer", GID_USER, cfg.username, sizeof(cfg.username) - 1);
    if (!g_user) goto fail;

    g_pass = prefsgui_field_string(&lay, "Passwort", GID_PASS, cfg.password, sizeof(cfg.password) - 1);
    if (!g_pass) goto fail;

    g_verifyssl = prefsgui_field_checkbox(&lay, "Zertifikat pruefen", GID_VERIFYSSL, cfg.verify_ssl);
    if (!g_verifyssl) goto fail;

    {
        WORD saved_w = lay.field_w;
        lay.field_w = 80;
        g_interval = prefsgui_field_integer(&lay, "Intervall (Min)", GID_INTERVAL, pl.interval_minutes);
        lay.field_w = saved_w;
    }
    if (!g_interval) goto fail;

    gad = prefsgui_status_line(&lay, 8, winw - 16, GID_STATUS, notice);
    if (!gad) goto fail;

    /* Folder-pair listview -- plain CreateGadget (not in prefsgui's
     * toolbox; see this file's own list-node helpers above), same shape
     * as amicloud-sync-cx/src/gui.c's read-only one but read-write here. */
    {
        struct NewGadget ng;
        memset(&ng, 0, sizeof(ng));
        ng.ng_TextAttr   = prefsgui_font();
        ng.ng_VisualInfo = prefsgui_vi();
        ng.ng_LeftEdge   = 8;
        ng.ng_TopEdge    = lay.y;
        ng.ng_Width      = winw - 16;
        ng.ng_Height     = 100;
        ng.ng_GadgetText = NULL;
        ng.ng_GadgetID   = GID_LISTVIEW;
        ng.ng_Flags      = 0;
        gad = CreateGadget(LISTVIEW_KIND, lay.context, &ng, GTLV_ReadOnly, (ULONG)TRUE, TAG_DONE);
        if (!gad) goto fail;
        g_listview   = gad;
        lay.context  = gad;
        lay.y       += 100 + 8;
    }

    /* Hinzufuegen/Bearbeiten/Entfernen row */
    {
        struct NewGadget ng;
        static const char *label[3] = { "Hinzuf\xfcgen", "Bearbeiten", "Entfernen" };
        static const UWORD ids[3]   = { GID_ADD, GID_EDIT, GID_REMOVE };
        WORD bw = (winw - 16 - 2 * lay.gap) / 3;
        int i;

        memset(&ng, 0, sizeof(ng));
        ng.ng_TextAttr   = prefsgui_font();
        ng.ng_VisualInfo = prefsgui_vi();
        ng.ng_TopEdge    = lay.y;
        ng.ng_Width      = bw;
        ng.ng_Height     = lay.row_h;
        ng.ng_Flags      = PLACETEXT_IN;

        for (i = 0; i < 3; i++) {
            ng.ng_LeftEdge   = 8 + i * (bw + lay.gap);
            ng.ng_GadgetText = (CONST_STRPTR)label[i];
            ng.ng_GadgetID   = ids[i];
            gad = CreateGadget(BUTTON_KIND, lay.context, &ng, TAG_DONE);
            if (!gad) goto fail;
            lay.context = gad;
        }
        lay.y += lay.row_h + lay.gap;
    }

    gad = prefsgui_use_save_cancel_row(&lay, 8, winw - 16, GID_USE, GID_SAVE, GID_CANCEL);
    if (!gad) goto fail;

    win = OpenWindowTags(NULL,
        WA_Left,      50,
        WA_Top,       30,
        WA_Width,     winw,
        WA_Height,    winh,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,   (ULONG)glist,
        WA_Title,     (ULONG)"AmiCloudSync-Einstellungen",
        WA_PubScreen, (ULONG)prefsgui_screen(),
        TAG_DONE);
    if (!win) goto fail;

    GT_RefreshWindow(win, NULL);

    build_lv_list(&pl);
    GT_SetGadgetAttrs(g_listview, win, NULL, GTLV_Labels, (ULONG)&g_lv_list, TAG_DONE);

    selected = -1;
    running  = 1;
    while (running) {
        UWORD code = 0;
        UWORD act  = prefsgui_wait_action(win, &code);

        switch (act) {
        case GID_LISTVIEW:
            selected = (int)code; /* code == selected entry's ordinal, or ~0 */
            break;

        case GID_VERIFYSSL:
            /* Tracked app-side, not read back via GT_GetGadgetAttrs --
             * see prefsgui_set_checkbox()'s doc comment in prefsgui.h. */
            cfg.verify_ssl = !cfg.verify_ssl;
            prefsgui_set_checkbox(win, g_verifyssl, cfg.verify_ssl);
            break;

        case GID_ADD: {
            SyncPair np;
            if (run_pair_dialog(NULL, &np)) {
                pairs_add(&pl, np.local, np.remote, np.enabled);
                refresh_listview(win, g_listview, &pl);
            }
            break;
        }

        case GID_EDIT:
            if (selected >= 0 && selected < pl.n) {
                SyncPair np;
                if (run_pair_dialog(&pl.items[selected], &np)) {
                    pl.items[selected] = np;
                    refresh_listview(win, g_listview, &pl);
                }
            }
            break;

        case GID_REMOVE:
            if (selected >= 0 && selected < pl.n) {
                pairs_remove(&pl, selected);
                selected = -1;
                refresh_listview(win, g_listview, &pl);
            }
            break;

        case GID_USE:
        case GID_SAVE:
            prefsgui_get_string(g_server, cfg.server,   sizeof(cfg.server));
            prefsgui_get_string(g_user,   cfg.username, sizeof(cfg.username));
            prefsgui_get_string(g_pass,   cfg.password, sizeof(cfg.password));
            pl.interval_minutes  = (int)prefsgui_get_integer(win, g_interval);

            pairs_save(PREFS_ENV, &cfg, &pl);
            if (act == GID_SAVE) pairs_save(PREFS_ENVARC, &cfg, &pl);
            running = 0;
            break;

        case GID_CANCEL:
        case PREFSGUI_ACT_CLOSE:
            running = 0;
            break;

        default:
            break;
        }
    }

    free_lv_list();
    CloseWindow(win);
    FreeGadgets(glist);
    pairs_free(&pl);
    prefsgui_screen_close();
    return 0;

fail:
    if (glist) FreeGadgets(glist);
    pairs_free(&pl);
    prefsgui_screen_close();
    return 1;
}
