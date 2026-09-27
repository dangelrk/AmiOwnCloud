/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef OWNCLOUDHANDLER_BACKEND_H
#define OWNCLOUDHANDLER_BACKEND_H

/*
 * backend.h -- handler-side half of the worker protocol. Forked unchanged
 * from handler/cloudhandler/src/backend.h (see ../../cloudhandler/
 * README.md); only the spawned binary name changed (backend.c) -- exec.
 * library only where it matters (backend_send(), called from the packet
 * loop); backend_init() is the one exception, called during
 * ACTION_STARTUP while dos.library is still open (see handler.c).
 */

#include "proto.h"

/* Locates a running owncloud-worker (FindPort) or spawns L:owncloud-worker
 * and waits (~30s, polling FindPort) for its port to appear. Must run
 * during ACTION_STARTUP, before dos.library is closed -- SystemTags()/
 * Open("NIL:", ...)/Delay() are all dos.library calls. Returns 1 on
 * success (backend_send() usable from here on), 0 on failure (the mount
 * should fail, see docs/handler-plan.md Stufe 2 "Worker-Start"). */
int backend_init(void);

/* Synchronous send/wait/reply -- blocks the whole handler until the
 * worker replies, up to a ~60s bounded timeout (BACKEND_TIMEOUT_SECS,
 * backend.c), or the worker's original unbounded WaitPort() if
 * timer.device somehow isn't available (accepted for Stage 2 v1, see
 * proto.h). Fills a reused, static WorkerMsg from the given fields (pass
 * 0/NULL for anything a given cmd doesn't use) and returns the finished
 * DOS error code from the reply (0 = success), or ERROR_LOCK_TIMEOUT if
 * the worker never replied in time. If reply is non-NULL, *reply is set
 * to that same reused WorkerMsg so the caller can read cmd-specific
 * output fields (entries/buf/mtime/...) -- valid only until the next
 * backend_send() call, and NULL on a timeout. The caller owns
 * FreeVec()-ing any worker-owned output buffer (entries/buf) per the
 * ownership rules in proto.h.
 *
 * Once one call times out, EVERY subsequent backend_send() call fails
 * immediately with ERROR_LOCK_TIMEOUT for the rest of this mount (see
 * backend.c's g_backend_dead) -- a still-outstanding worker reply could
 * write into the reused WorkerMsg at any later time, so it can never be
 * safely reused. This matches the already-documented "worker died/wedged
 * mid-session -> remount to recover" limitation (README, Stage 2 test 8);
 * a timeout just makes that failure immediate and clean instead of an
 * indefinite freeze of the whole CLOUD: volume. */
LONG backend_send(WReqCmd cmd, const char *path, const char *path2,
                  UBYTE *buf, ULONG buflen,
                  const struct DateStamp *mtime, LONG protection, const char *comment,
                  WorkerMsg **reply);

/* Crash-hunt diagnostic (temporary, see vol.h's is_diaglog comment): a
 * plain static ring buffer backend_send() appends fixed trace lines to at
 * each step of PutMsg/Wait/GetMsg/AbortIO/WaitIO -- exec.library only, no
 * dos.library, safe to call from the packet loop. Exposed read-only here
 * so actions.c's do_find() can serve it as the synthetic "debuglog" file's
 * content instead of a real WREQ_GET. */
extern char  g_diag_buf[4096];
extern ULONG g_diag_pos;

#endif /* OWNCLOUDHANDLER_BACKEND_H */
