/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * gui.c -- see gui.h.
 */
#include "gui.h"

#include <exec/types.h>
#include <exec/lists.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct Library        *GadToolsBase;

/* Gadget text is deliberately topaz.font/8, not g_screen->Font -- see
 * core/common/src/prefsgui.c's identical comment (the Prefs programs
 * share this exact rationale: a wider Workbench screen font made every
 * button/label here render far too wide, reported after real hardware/
 * emulator testing). topaz.font/8-9 is a built-in ROM font, no
 * OpenFont()/CloseFont() needed -- GadTools resolves it from this
 * TextAttr internally. */
static struct TextAttr g_topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

/* NewList()/AddTail()/RemHead() are amiga.lib (alib) helpers, not real
 * exec.library calls (see clib/alib_protos.h) -- hand-rolled here rather
 * than linking amiga.lib for three trivial pointer-chases (this app
 * needs it for nothing else; MUI apps in this repo link -lamiga purely
 * for DoMethod()/DoSuperMethod(), which GadTools never uses). */
static void ac_list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail      = NULL;
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

#define GID_LISTVIEW 1
#define GID_SYNCONE  2
#define GID_SYNCALL  3
#define GID_REFRESH  4
#define GID_STATUS   5
#define GID_MAX      6

#define MID_ABOUT ((APTR)1)
#define MID_QUIT  ((APTR)2)

static struct Screen  *g_screen;
static APTR             g_vi;
static struct Window   *g_win;
static struct Gadget   *g_glist;
static struct Gadget   *g_gad[GID_MAX];
static struct Menu     *g_menu;
static struct List      g_lv_list;
static int              g_lv_built;
static char             g_status_buf[256];

/* ------------------------------------------------------------------ */
/* Listview backing list -- exec List of malloc'd Nodes, one per pair,   */
/* rebuilt on every refresh (small lists, simplicity over micro-opt).    */
/* ------------------------------------------------------------------ */

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

void gui_refresh_list(const PairList *pl)
{
    if (!g_win) return;
    /* Detach the old list before touching it -- the documented GadTools
     * idiom for safely swapping a listview's backing list at runtime. */
    GT_SetGadgetAttrs(g_gad[GID_LISTVIEW], g_win, NULL, GTLV_Labels, (ULONG)~0, TAG_DONE);
    free_lv_list();
    build_lv_list(pl);
    GT_SetGadgetAttrs(g_gad[GID_LISTVIEW], g_win, NULL, GTLV_Labels, (ULONG)&g_lv_list, TAG_DONE);
}

void gui_set_status(const char *text)
{
    strncpy(g_status_buf, text ? text : "", sizeof(g_status_buf) - 1);
    g_status_buf[sizeof(g_status_buf) - 1] = '\0';
    if (!g_win) return;
    GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, NULL, GTTX_Text, (ULONG)g_status_buf, TAG_DONE);
}

/* ------------------------------------------------------------------ */
/* Main window                                                           */
/* ------------------------------------------------------------------ */

static struct NewMenu g_newmenu[] = {
    { NM_TITLE, (CONST_STRPTR)"Projekt",   NULL,               0, 0, NULL },
    { NM_ITEM,  (CONST_STRPTR)"Info...",   (CONST_STRPTR)"?",  0, 0, MID_ABOUT },
    { NM_ITEM,  (CONST_STRPTR)NM_BARLABEL, NULL,               0, 0, NULL },
    { NM_ITEM,  (CONST_STRPTR)"Beenden",   (CONST_STRPTR)"Q",  0, 0, MID_QUIT },
    { NM_END,   NULL,                      NULL,               0, 0, NULL }
};

void gui_close(void)
{
    free_lv_list();
    if (g_win)   { ClearMenuStrip(g_win); CloseWindow(g_win); g_win = NULL; }
    if (g_menu)  { FreeMenus(g_menu); g_menu = NULL; }
    if (g_glist) { FreeGadgets(g_glist); g_glist = NULL; }
    if (g_vi)    { FreeVisualInfo(g_vi); g_vi = NULL; }
    if (g_screen) { UnlockPubScreen(NULL, g_screen); g_screen = NULL; }
    if (GadToolsBase) { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

int gui_is_open(void) { return g_win != NULL; }

int gui_open(const PairList *pl, const char *title)
{
    struct NewGadget ng;
    struct Gadget *gad;
    WORD topborder;
    int winw, winh;
    int bx, by, bw, bh, gap, i;
    static const char *btn_label[3] = { "Jetzt syncen", "Alle syncen", "Neu laden" };
    static const int   btn_gid[3]   = { GID_SYNCONE, GID_SYNCALL, GID_REFRESH };

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37L);
    if (!IntuitionBase) return 0;
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37L);
    if (!GadToolsBase) { gui_close(); return 0; }

    g_screen = LockPubScreen(NULL);
    if (!g_screen) { gui_close(); return 0; }

    g_vi = GetVisualInfo(g_screen, TAG_DONE);
    if (!g_vi) { gui_close(); return 0; }

    g_glist = NULL;
    gad = CreateContext(&g_glist);
    if (!gad) { gui_close(); return 0; }

    topborder = g_screen->WBorTop + g_screen->Font->ta_YSize + 2;
    /* Window width no longer sized for 5 buttons -- kept close to the
     * other Prefs windows' 460px anyway, so the status line still has
     * room for a full "Work:Dokumente: 3 hoch, 2 runter, 0 Konflikt(e)"
     * message. */
    bh = 20; gap = 6;
    winw = 460;
    bw = (winw - 16 - 2 * gap) / 3;
    winh = topborder + 8 + 100 + 8 + bh + 8 + 12 + 8;

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = &g_topaz8;
    ng.ng_VisualInfo = g_vi;

    ng.ng_LeftEdge = 8; ng.ng_TopEdge = topborder + 8;
    ng.ng_Width    = winw - 16; ng.ng_Height = 100;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_LISTVIEW; ng.ng_Flags = 0;
    /* NOT GTLV_ReadOnly -- "read-only" here (see this file's own header
     * comment) means the PAIR LIST CONTENTS can't be edited from this
     * window (no add/edit/remove -- that moved to amiowncloud-sync-prefs),
     * not that an entry can't be SELECTED. GTLV_ReadOnly disables click
     * selection entirely (see gadtools.doc), which broke "Jetzt syncen"
     * (it needs s_selected, see gui_poll() below) -- the status line
     * telling the user to pick a pair first was correct, the gadget
     * just never let them. */
    gad = CreateGadget(LISTVIEW_KIND, gad, &ng, TAG_DONE);
    if (!gad) { gui_close(); return 0; }
    g_gad[GID_LISTVIEW] = gad;

    by = topborder + 8 + 100 + 8;
    bx = 8;
    for (i = 0; i < 3; i++) {
        ng.ng_LeftEdge = bx + i * (bw + gap); ng.ng_TopEdge = by;
        ng.ng_Width = bw; ng.ng_Height = bh;
        ng.ng_GadgetText = (CONST_STRPTR)btn_label[i];
        ng.ng_GadgetID   = btn_gid[i];
        ng.ng_Flags      = PLACETEXT_IN;
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_DONE);
        if (!gad) { gui_close(); return 0; }
        g_gad[btn_gid[i]] = gad;
    }

    ng.ng_LeftEdge = 8; ng.ng_TopEdge = by + bh + 8;
    ng.ng_Width = winw - 16; ng.ng_Height = 12;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_STATUS; ng.ng_Flags = PLACETEXT_IN;
    gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)"", TAG_DONE);
    if (!gad) { gui_close(); return 0; }
    g_gad[GID_STATUS] = gad;

    g_win = OpenWindowTags(NULL,
        WA_Left,        40,
        WA_Top,         40,
        WA_Width,       winw,
        WA_Height,      winh,
        WA_IDCMP,       IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP | IDCMP_MENUPICK,
        WA_Flags,       WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,     (ULONG)g_glist,
        WA_Title,       (ULONG)title,
        WA_PubScreen,   (ULONG)g_screen,
        TAG_DONE);
    if (!g_win) { gui_close(); return 0; }

    GT_RefreshWindow(g_win, NULL);

    g_menu = CreateMenus(g_newmenu, GTMN_FrontPen, 0L, TAG_DONE);
    if (g_menu && LayoutMenus(g_menu, g_vi, GTMN_NewLookMenus, (ULONG)TRUE, TAG_DONE)) {
        SetMenuStrip(g_win, g_menu);
    }

    build_lv_list(pl);
    GT_SetGadgetAttrs(g_gad[GID_LISTVIEW], g_win, NULL, GTLV_Labels, (ULONG)&g_lv_list, TAG_DONE);
    gui_set_status("Bereit.");

    return 1;
}

void gui_show(void)
{
    if (g_win) WindowToFront(g_win);
}

void gui_hide(void)
{
    /* No separate hidden state today -- the CxBroker's COF_SHOW_HIDE
     * flag means Exchange itself only ever asks main.c to open/close
     * the window outright (see main.c); "hide" and "close" are the same
     * operation for this simple v1. */
}

ULONG gui_sigmask(void)
{
    return g_win ? (1UL << g_win->UserPort->mp_SigBit) : 0;
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                       */
/* ------------------------------------------------------------------ */

GuiAction gui_poll(PairList *pl, int *out_selected)
{
    struct IntuiMessage *msg;
    GuiAction action = GUI_ACT_NONE;
    static int s_selected = -1; /* remembered across calls (GadTools
                                  * doesn't resend the current selection
                                  * unless the user actually clicks it) */

    if (!g_win) return GUI_ACT_NONE;

    while ((msg = GT_GetIMsg(g_win->UserPort)) != NULL) {
        ULONG class = msg->Class;
        UWORD code  = msg->Code;
        struct Gadget *gad = (struct Gadget *)msg->IAddress;
        GT_ReplyIMsg(msg);

        if (class == IDCMP_CLOSEWINDOW) {
            action = GUI_ACT_CLOSE;
        } else if (class == IDCMP_MENUPICK) {
            struct MenuItem *item = ItemAddress(g_menu, code);
            if (item) {
                APTR ud = GTMENUITEM_USERDATA(item);
                if (ud == MID_QUIT) action = GUI_ACT_CLOSE;
                else if (ud == MID_ABOUT)
                    gui_set_status("AmiOwnCloudSync -- OwnCloud-Ordnerabgleich fuer AmigaOS");
            }
        } else if (class == IDCMP_GADGETUP) {
            if (gad == g_gad[GID_LISTVIEW]) {
                s_selected = (int)code; /* code == selected entry's ordinal, or ~0 */
            } else if (gad == g_gad[GID_SYNCONE]) {
                if (s_selected >= 0 && s_selected < pl->n) {
                    if (out_selected) *out_selected = s_selected;
                    action = GUI_ACT_SYNC_ONE;
                } else {
                    gui_set_status("Bitte zuerst ein Ordnerpaar in der Liste ausw\xe4hlen.");
                }
            } else if (gad == g_gad[GID_SYNCALL]) {
                action = GUI_ACT_SYNC_ALL;
            } else if (gad == g_gad[GID_REFRESH]) {
                action = GUI_ACT_REFRESH;
            }
        }
    }

    return action;
}
