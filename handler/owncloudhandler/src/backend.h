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

/* Locates a running amiowncloud-worker (FindPort) or spawns L:amiowncloud-worker
 * and waits (~30s, polling FindPort) for its port to appear. Must run
 * during ACTION_STARTUP, before dos.library is closed -- SystemTags()/
 * Open("NIL:", ...)/Delay() are all dos.library calls. Returns 1 on
 * success (backend_send() usable from here on), 0 on failure (the mount
 * should fail, see docs/handler-plan.md Stufe 2 "Worker-Start"). */
int backend_init(void);

/* Synchronous send/wait/reply: one request at a time, waits until the
 * worker replies (no handler-side timeout -- the worker's HTTP layer
 * has idle timeouts, see backend.c). While waiting, the busy hook (see
 * backend_set_busy_hook()) keeps answering packets that need no
 * network. Fills a reused, static WorkerMsg from the given fields (pass
 * 0/NULL for anything a given cmd doesn't use) and returns the finished
 * DOS error code from the reply (0 = success). If reply is non-NULL,
 * *reply is set to that same reused WorkerMsg so the caller can read
 * cmd-specific output fields (entries/buf/mtime/...) -- valid only
 * until the next backend_send() call. The caller owns FreeVec()-ing any
 * worker-owned output buffer (entries/buf) per the ownership rules in
 * proto.h. Never call it from the busy hook. */
LONG backend_send(WReqCmd cmd, const char *path, const char *path2,
                  UBYTE *buf, ULONG buflen,
                  const struct DateStamp *mtime, LONG protection, const char *comment,
                  WorkerMsg **reply);

/* WREQ_READ: bytes offset..offset+len-1 of path (fewer at the end of
 * the file); *reply's buf/buflen hold them, owned by the caller exactly
 * like a WREQ_GET body. Same waiting rules as backend_send(). */
LONG backend_read(const char *path, LONG offset, ULONG len, WorkerMsg **reply);

/* Crash-hunt diagnostic (temporary, see vol.h's is_diaglog comment): a
 * plain static ring buffer backend_send() appends fixed trace lines to at
 * each step of PutMsg/Wait/GetMsg/AbortIO/WaitIO -- exec.library only, no
 * dos.library, safe to call from the packet loop. Exposed read-only here
 * so actions.c's do_find() can serve it as the synthetic "debuglog" file's
 * content instead of a real WREQ_GET. */
extern char  g_diag_buf[4096];

/* While backend_send() waits for the worker it also wakes up on port's
 * signal and calls hook(), which must answer or queue whatever packets
 * arrived there -- without ever calling backend_send() itself (see
 * handler.c's serve_while_busy()). */
void backend_set_busy_hook(struct MsgPort *port, void (*hook)(void));

/* Highest worker clock seen in any reply, and the configured directory
 * cache lifetime (0 until the first reply) -- see WorkerMsg. */
LONG backend_now(void);
LONG backend_cache_ttl(void);
extern ULONG g_diag_pos;

#endif /* OWNCLOUDHANDLER_BACKEND_H */
