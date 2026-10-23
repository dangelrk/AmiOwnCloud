/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef AC_SYNC_CX_COMMODITY_H
#define AC_SYNC_CX_COMMODITY_H

/*
 * commodity.h -- CxBroker registration + Exchange integration (show up
 * in Exchange, respond to its Enable/Disable/Appear/Disappear/Kill
 * gadgets) plus one HotKey that toggles the pair-list window. See
 * AmigaDoku/rkrm/RKRM_Libs_prgs/commodities/{broker,HotKey}.c for the
 * canonical shape this follows.
 */

#include <exec/types.h>

typedef enum {
    CX_ACTION_NONE = 0,
    CX_ACTION_TOGGLE_WINDOW, /* HotKey pressed */
    CX_ACTION_SHOW_WINDOW,   /* Exchange "Appear", or CXCMD_UNIQUE (user
                              * tried to launch a second copy) */
    CX_ACTION_HIDE_WINDOW,   /* Exchange "Disappear" */
    CX_ACTION_QUIT           /* Exchange "Kill" */
} CxAction;

/* hotkey is an InputXpression description string, e.g. "ctrl alt a"
 * (NULL/"" disables the hotkey -- Exchange's Appear/Disappear and the
 * window's own gadgets remain the only way to show/hide it). Returns 1
 * on success; ac_commodity_close() is always safe to call afterwards
 * even on failure (cleans up whatever partial state exists). */
int ac_commodity_open(const char *broker_name, const char *broker_title,
                      const char *broker_descr, const char *hotkey);
void ac_commodity_close(void);

/* Why the last ac_commodity_open() failed, for a requester -- or NULL
 * when it failed only because another AmiOwnCloudSync is already running
 * (NBU_UNIQUE|NBU_NOTIFY: that instance has been told to show its
 * window, so the new one should just quit quietly). */
const char *ac_commodity_error(void);

/* The signal bit main.c's Wait() should include for the broker port. */
ULONG ac_commodity_sigmask(void);

/* Drains and replies to every pending CxMsg, applying ENABLE/DISABLE
 * internally (ActivateCxObj) without surfacing them, and returns the
 * single most urgent user-facing action from this batch -- QUIT beats
 * everything else, SHOW/HIDE beat a mere TOGGLE. */
CxAction ac_commodity_poll(void);

#endif /* AC_SYNC_CX_COMMODITY_H */
