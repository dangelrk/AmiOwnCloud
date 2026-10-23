/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * commodity.c -- see commodity.h.
 */
#include "commodity.h"

#include <exec/types.h>
#include <exec/ports.h>
#include <libraries/commodities.h>
#include <proto/exec.h>
#include <proto/commodities.h>

#include <stddef.h>
#include <stdio.h>

struct Library *CxBase;

/* Custom CXM_IEVENT id our own HotKey sender tags its messages with --
 * arbitrary, just needs to not collide with any CXCMD_* value. */
#define EVT_HOTKEY 1000L

static struct MsgPort *g_broker_port;
static CxObj          *g_broker;
static CxObj           *g_filter;
static CxObj           *g_sender;
static CxObj           *g_translate;

static struct NewBroker g_nb;

static const char *g_error;
static char        g_error_buf[160];

const char *ac_commodity_error(void)
{
    return g_error;
}

static void cleanup_partial(void)
{
    if (g_broker) {
        DeleteCxObjAll(g_broker); /* also tears down filter/sender/translate */
        g_broker = NULL;
    }
    g_filter = g_sender = g_translate = NULL;
    if (g_broker_port) {
        struct Message *msg;
        while ((msg = GetMsg(g_broker_port)) != NULL) ReplyMsg(msg);
        DeleteMsgPort(g_broker_port);
        g_broker_port = NULL;
    }
}

int ac_commodity_open(const char *broker_name, const char *broker_title,
                      const char *broker_descr, const char *hotkey)
{
    LONG err = 0;

    g_error = "Unbekannter Fehler beim Anmelden der Commodity.";
    CxBase = OpenLibrary((CONST_STRPTR)"commodities.library", 37L);
    if (!CxBase) {
        g_error = "commodities.library V37 kann nicht geoeffnet werden.";
        return 0;
    }

    g_broker_port = CreateMsgPort();
    if (!g_broker_port) {
        g_error = "Nicht genug Speicher (MsgPort).";
        return 0;
    }

    g_nb.nb_Version = NB_VERSION;
    g_nb.nb_Name    = (STRPTR)broker_name;
    g_nb.nb_Title   = (STRPTR)broker_title;
    g_nb.nb_Descr   = (STRPTR)broker_descr;
    g_nb.nb_Unique  = NBU_UNIQUE | NBU_NOTIFY;
    g_nb.nb_Flags   = COF_SHOW_HIDE;
    g_nb.nb_Pri     = 0;
    g_nb.nb_Port    = g_broker_port;
    g_nb.nb_ReservedChannel = 0;

    g_broker = CxBroker(&g_nb, &err);
    if (!g_broker) {
        g_error = (err == CBERR_DUP) ? NULL
                : "Die Commodity konnte nicht angemeldet werden.";
        cleanup_partial();
        return 0;
    }

    if (hotkey && hotkey[0]) {
        g_filter = CxFilter((CONST_STRPTR)hotkey);
        if (!g_filter) { cleanup_partial(); return 0; }
        AttachCxObj(g_broker, g_filter);

        g_sender = CxSender(g_broker_port, EVT_HOTKEY);
        if (!g_sender) { cleanup_partial(); return 0; }
        AttachCxObj(g_filter, g_sender);

        g_translate = CxTranslate(NULL);
        if (!g_translate) { cleanup_partial(); return 0; }
        AttachCxObj(g_filter, g_translate);

        if (CxObjError(g_filter) != 0) {
            snprintf(g_error_buf, sizeof(g_error_buf),
                     "Ungueltiger Hotkey \"%s\" (Tooltype CX_POPKEY).", hotkey);
            g_error = g_error_buf;
            cleanup_partial();
            return 0;
        }
    }

    ActivateCxObj(g_broker, 1L);
    g_error = NULL;
    return 1;
}

void ac_commodity_close(void)
{
    cleanup_partial();
    if (CxBase) { CloseLibrary(CxBase); CxBase = NULL; }
}

ULONG ac_commodity_sigmask(void)
{
    return g_broker_port ? (1UL << g_broker_port->mp_SigBit) : 0;
}

CxAction ac_commodity_poll(void)
{
    CxMsg *msg;
    int want_quit = 0, want_show = 0, want_hide = 0, want_toggle = 0;

    while ((msg = (CxMsg *)GetMsg(g_broker_port)) != NULL) {
        ULONG type = CxMsgType(msg);
        LONG  id   = CxMsgID(msg);
        ReplyMsg((struct Message *)msg);

        if (type == CXM_IEVENT) {
            if (id == EVT_HOTKEY) want_toggle = 1;
        } else if (type == CXM_COMMAND) {
            switch (id) {
            case CXCMD_DISABLE:   ActivateCxObj(g_broker, 0L); break;
            case CXCMD_ENABLE:    ActivateCxObj(g_broker, 1L); break;
            case CXCMD_APPEAR:    want_show = 1; break;
            case CXCMD_DISAPPEAR: want_hide = 1; break;
            case CXCMD_UNIQUE:    want_show = 1; break; /* user tried to launch a 2nd copy */
            case CXCMD_KILL:      want_quit = 1; break;
            default: break;
            }
        }
    }

    if (want_quit) return CX_ACTION_QUIT;
    if (want_show) return CX_ACTION_SHOW_WINDOW;
    if (want_hide) return CX_ACTION_HIDE_WINDOW;
    if (want_toggle) return CX_ACTION_TOGGLE_WINDOW;
    return CX_ACTION_NONE;
}
