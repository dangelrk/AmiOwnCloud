/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef AC_SYNC_CX_GUI_H
#define AC_SYNC_CX_GUI_H

/*
 * gui.h -- the pair-list window: a READ-ONLY GadTools listview +
 * Jetzt syncen/Alle syncen/Neu laden buttons + a status line, plus a
 * small GadTools menu (Projekt: Info/Beenden). GadTools rather than
 * full ReAction (BOOPSI window.class/listbrowser.class) -- lighter on
 * the 4 MB target, and explicitly one of the UI options this plan
 * allows (see the plan's own "Intuition/ReAction/GadTools").
 *
 * Folder-pair editing (Hinzufuegen/Bearbeiten/Entfernen) has moved to
 * the amiowncloud-sync-prefs program (see repo plan "Drei getrennte
 * Prefs-Programme") -- this window only ever displays pl, never
 * mutates it; it knows nothing about libcloudsync either, so
 * SYNC_ONE/SYNC_ALL/REFRESH are only ever reported back to main.c to
 * actually execute (REFRESH: re-read AmiOwnCloudSync.cfg from disk --
 * main.c already does this automatically on window-open and before
 * every autosync tick, see reload_prefs(); this button is for "I just
 * saved in AmiOwnCloudSyncPrefs and want THIS already-open window to
 * notice right now", without closing/reopening it or waiting).
 */

#include <exec/types.h>
#include "pairs.h"

typedef enum {
    GUI_ACT_NONE = 0,
    GUI_ACT_SYNC_ONE, /* *out_selected is the pair index to sync */
    GUI_ACT_SYNC_ALL,
    GUI_ACT_REFRESH,  /* re-read AmiOwnCloudSync.cfg now (see above) */
    GUI_ACT_CLOSE     /* close gadget, or the menu's "Beenden" */
} GuiAction;

int  gui_open(const PairList *pl, const char *title);
void gui_close(void);
int  gui_is_open(void);
void gui_show(void);
void gui_hide(void);
ULONG gui_sigmask(void);

/* Processes pending IDCMP messages against pl (Add/Edit/Remove already
 * applied in place by the time this returns) and returns the single
 * action worth acting on this batch. *out_selected receives the
 * currently-selected pair's index for GUI_ACT_SYNC_ONE (undefined
 * otherwise). */
GuiAction gui_poll(PairList *pl, int *out_selected);

/* Re-reads pl into the listview -- call after main.c changes pl itself
 * (e.g. after a sync updates nothing visible today, but keeps the door
 * open for a future "last synced" column). */
void gui_refresh_list(const PairList *pl);

void gui_set_status(const char *text);

#endif /* AC_SYNC_CX_GUI_H */
