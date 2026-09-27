/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * timer.c -- see timer.h.
 */
#include "timer.h"

#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>

static struct MsgPort     *g_timer_port;
static struct timerequest *g_timer_req;

int ac_timer_open(void)
{
    g_timer_port = CreateMsgPort();
    if (!g_timer_port) return 0;

    g_timer_req = (struct timerequest *)CreateIORequest(g_timer_port, sizeof(struct timerequest));
    if (!g_timer_req) {
        DeleteMsgPort(g_timer_port);
        g_timer_port = NULL;
        return 0;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)g_timer_req, 0) != 0) {
        DeleteIORequest((APTR)g_timer_req);
        DeleteMsgPort(g_timer_port);
        g_timer_req  = NULL;
        g_timer_port = NULL;
        return 0;
    }
    return 1;
}

void ac_timer_close(void)
{
    if (!g_timer_req) return;

    if (!CheckIO((struct IORequest *)g_timer_req)) {
        AbortIO((struct IORequest *)g_timer_req);
        WaitIO((struct IORequest *)g_timer_req);
    }
    CloseDevice((struct IORequest *)g_timer_req);
    DeleteIORequest((APTR)g_timer_req);
    DeleteMsgPort(g_timer_port);
    g_timer_req  = NULL;
    g_timer_port = NULL;
}

void ac_timer_start(long seconds)
{
    if (!g_timer_req) return;
    g_timer_req->tr_node.io_Command = TR_ADDREQUEST;
    g_timer_req->tr_time.tv_secs  = (ULONG)seconds;
    g_timer_req->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)g_timer_req);
}

ULONG ac_timer_sigmask(void)
{
    return g_timer_port ? (1UL << g_timer_port->mp_SigBit) : 0;
}

void ac_timer_consume(void)
{
    struct Message *msg;
    while ((msg = GetMsg(g_timer_port)) != NULL) { /* g_timer_req IS the message; nothing else to do */ }
}
