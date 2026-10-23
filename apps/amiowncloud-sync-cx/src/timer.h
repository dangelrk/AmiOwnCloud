/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef AC_SYNC_CX_TIMER_H
#define AC_SYNC_CX_TIMER_H

/*
 * timer.h -- periodic autosync tick via timer.device (UNIT_VBLANK).
 *
 * Deliberately the "one persistent connection, one IORequest reused for
 * the whole run" idiom -- NOT the OpenDevice()/CloseDevice()-per-call
 * cycling that handler/cloudhandler/src/backend.c documents hitting a
 * real timer.device bug under AmiBerry emulation (a wedged cross-process
 * reply race). This shape is the standard one every well-behaved Amiga
 * clock/commodity already uses, and doesn't share that bug's specific
 * trigger (repeated OpenDevice/CloseDevice on a persistent port) --  but
 * see main.c's Wait() loop for the fallback if ac_timer_open() ever does
 * fail: autosync simply never fires, manual "Jetzt syncen" still works.
 */

#include <exec/types.h>

int   ac_timer_open(void);
void  ac_timer_close(void);

/* Issues a fresh one-shot request for `seconds` from now. Call once
 * after ac_timer_open() and again every time main.c's Wait() loop
 * consumes the previous tick (ac_timer_consume()) -- never while a
 * request is still outstanding. */
void  ac_timer_start(long seconds);

/* The signal bit main.c's Wait() should include for the timer -- 0 if
 * ac_timer_open() failed or wasn't called. */
ULONG ac_timer_sigmask(void);

/* Drains the completed tick after Wait() reports the timer signal, so
 * the port is clean before the next ac_timer_start(). */
void  ac_timer_consume(void);

#endif /* AC_SYNC_CX_TIMER_H */
