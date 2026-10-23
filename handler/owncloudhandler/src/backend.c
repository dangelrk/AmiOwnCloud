/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "backend.h"
#include "util.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

static struct MsgPort *g_worker_port;
static struct MsgPort *g_reply_port;
static WorkerMsg        g_req;

/* Crash-hunt diagnostic ring buffer (temporary, see backend.h/vol.h) --
 * plain static array, exec.library only, safe from the packet loop.
 * Retrieved afterward by reading the synthetic "debuglog" file (see
 * actions.c's do_find()), since the handler can never safely write a real
 * log file to disk from in here. */
char  g_diag_buf[4096];
ULONG g_diag_pos;

static void diag_log(const char *msg)
{
    ULONG n = u_strlen(msg);
    if (g_diag_pos + n >= sizeof(g_diag_buf)) return; /* full -- drop silently */
    u_memcpy(g_diag_buf + g_diag_pos, msg, n);
    g_diag_pos += n;
}

/* Appends label followed by val as decimal (with an optional leading '-')
 * and a newline -- e.g. diag_logn("cmd=", 2) -> "cmd=2\n". No snprintf
 * available here (-nostdlib, see util.h's file header). */
static void diag_logn(const char *label, LONG val)
{
    char buf[16];
    int i = 0;
    ULONG uval;
    int neg = 0;

    diag_log(label);
    if (val < 0) { neg = 1; uval = (ULONG)(-val); } else { uval = (ULONG)val; }
    do { buf[i++] = (char)('0' + (uval % 10)); uval /= 10; } while (uval && i < (int)sizeof(buf) - 1);
    if (neg && i < (int)sizeof(buf) - 1) buf[i++] = '-';
    while (i > 0) {
        char c[2];
        c[0] = buf[--i];
        c[1] = '\0';
        diag_log(c);
    }
    diag_log("\n");
}

/* No handler-side timeout on purpose. An earlier version bounded the
 * wait with timer.device (60 s, then the mount was dead for good) --
 * the wrong layer: at modem speed a legitimate 200 KB read already takes
 * longer, and that timer also misfired on every second call (most
 * likely a stale timer signal left behind by AbortIO()/WaitIO()). The
 * worker's HTTP layer has proper idle timeouts instead (config
 * timeout=, default 120 s: fails only when nothing moves at all), so a
 * reply always comes; a worker that crashes outright still needs a
 * remount (README, Stage 2 test 8).
 *
 * While waiting, packets that need no network are still answered (see
 * backend_set_busy_hook()), so one long transfer no longer freezes
 * Workbench's view of the whole volume. */
static struct MsgPort *g_busy_port;
static void          (*g_busy_hook)(void);

/* Latest worker clock and configured cache lifetime, from every reply
 * (WorkerMsg.server_now/cache_ttl). */
static LONG g_backend_now;
static LONG g_backend_cache_ttl;

void backend_set_busy_hook(struct MsgPort *port, void (*hook)(void))
{
    g_busy_port = port;
    g_busy_hook = hook;
}

LONG backend_now(void)       { return g_backend_now; }
LONG backend_cache_ttl(void) { return g_backend_cache_ttl; }

/* Crash-isolation build (user's suggestion): stripped out the worker
 * entirely -- no AmiSSL, no bsdsocket, no second process, no PutMsg/Wait
 * at all for WREQ_LIST -- and fabricated a fixed, fake directory listing
 * straight into g_req instead. This was the test that conclusively proved
 * the crash lived entirely in the handler process (it still crashed with
 * zero worker/network involvement), which led to finding the real bug:
 * _start() wasn't at hunk offset 0 (handler.c's file header). Left here,
 * disabled, in case this kind of isolation is ever needed again. */
#define AC_BACKEND_STUB_MODE 0

int backend_init(void)
{
#if AC_BACKEND_STUB_MODE
    /* No worker, no reply port even needed at all in stub mode -- see
     * backend_send() below. */
    return 1;
#else
    ULONG tries;
    BPTR nilIn, nilOut;
    LONG started;

    g_reply_port = CreateMsgPort();
    if (!g_reply_port) return 0;

    diag_log("backend_init: g_reply_port=");
    diag_logn("", (LONG)g_reply_port);
    diag_log("backend_init: sizeof(WorkerMsg)=");
    diag_logn("", (LONG)sizeof(WorkerMsg));
    diag_log("backend_init: sizeof(WEntry)=");
    diag_logn("", (LONG)sizeof(WEntry));
    diag_log("backend_init: offsetof(buf)=");
    diag_logn("", (LONG)((char *)&g_req.buf - (char *)&g_req));
    diag_log("backend_init: offsetof(result)=");
    diag_logn("", (LONG)((char *)&g_req.result - (char *)&g_req));


    g_worker_port = FindPort((CONST_STRPTR)AC_WORKER_PORT);
    if (g_worker_port) return 1; /* already running -- manually started, see README */

    nilIn  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    nilOut = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!nilIn || !nilOut) {
        if (nilIn)  Close(nilIn);
        if (nilOut) Close(nilOut);
        return 0;
    }

    /* SYS_Asynch closes nilIn/nilOut itself once the worker exits -- do
     * NOT close them here on the success path, ownership has transferred. */
    started = SystemTags((CONST_STRPTR)"L:amiowncloud-worker",
                          SYS_Asynch,   (ULONG)TRUE,
                          SYS_Input,    (ULONG)nilIn,
                          SYS_Output,   (ULONG)nilOut,
                          NP_StackSize, (ULONG)65536,
                          TAG_DONE);
    if (started == -1) {
        /* Launch failed outright -- never became a process, so ownership
         * of the streams never transferred; we must close them. */
        Close(nilIn);
        Close(nilOut);
        return 0;
    }

    for (tries = 0; tries < 150; tries++) { /* ~30s at Delay(10) == 0.2s each */
        g_worker_port = FindPort((CONST_STRPTR)AC_WORKER_PORT);
        if (g_worker_port) return 1;
        Delay(10);
    }
    return 0;
}
#endif /* AC_BACKEND_STUB_MODE */

/* Offset for the next request, see backend_read(). */
static LONG g_next_offset;

LONG backend_read(const char *path, LONG offset, ULONG len, WorkerMsg **reply)
{
    g_next_offset = offset;
    return backend_send(WREQ_READ, path, (const char *)0, (UBYTE *)0, len,
                        (const struct DateStamp *)0, 0, (const char *)0, reply);
}

LONG backend_send(WReqCmd cmd, const char *path, const char *path2,
                  UBYTE *buf, ULONG buflen,
                  const struct DateStamp *mtime, LONG protection, const char *comment,
                  WorkerMsg **reply)
{
    u_memset(&g_req, 0, sizeof(g_req));
    g_req.offset  = g_next_offset;
    g_next_offset = 0;
    g_req.msg.mn_ReplyPort = g_reply_port;
    g_req.msg.mn_Length    = sizeof(g_req);

    g_req.cmd = cmd;
    if (path)    u_strncpy(g_req.path,  path,  sizeof(g_req.path));
    if (path2)   u_strncpy(g_req.path2, path2, sizeof(g_req.path2));
    g_req.buf        = buf;
    g_req.buflen      = buflen;
    if (mtime) u_memcpy(&g_req.mtime, mtime, sizeof(struct DateStamp));
    g_req.protection  = protection;
    if (comment) u_strncpy(g_req.comment, comment, sizeof(g_req.comment));

#if AC_BACKEND_STUB_MODE
    /* No worker at all in stub mode (see backend_init()/the big comment
     * above) -- fabricate a fixed, fake reply straight into g_req instead
     * of ever touching g_worker_port/PutMsg/Wait. Only WREQ_LIST is
     * stubbed (that's all a plain "Dir OWNCLOUD:" needs); anything else just
     * fails cleanly. */
    if (cmd == WREQ_LIST) {
        WEntry *fake = (WEntry *)AllocVec(3 * sizeof(WEntry), MEMF_PUBLIC | MEMF_CLEAR);
        if (fake) {
            u_strncpy(fake[0].name, "stub-file.txt", sizeof(fake[0].name));
            fake[0].is_dir = 0;
            fake[0].size   = 12345;
            u_strncpy(fake[1].name, "stub-dir", sizeof(fake[1].name));
            fake[1].is_dir = 1;
            u_strncpy(fake[2].name, "stub2.txt", sizeof(fake[2].name));
            fake[2].is_dir = 0;
            fake[2].size   = 42;
        }
        g_req.entries    = fake;
        g_req.n_entries  = fake ? 3 : 0;
        g_req.server_now = 0;
        g_req.result     = 0;
    } else {
        g_req.result = ERROR_OBJECT_NOT_FOUND;
    }
    if (reply) *reply = &g_req;
    return g_req.result;
#endif /* AC_BACKEND_STUB_MODE */

    diag_log("backend_send: PutMsg cmd=");
    diag_logn("", (LONG)cmd);
    PutMsg(g_worker_port, (struct Message *)&g_req);
    diag_log("backend_send: PutMsg done\n");

    {
        ULONG replySig = 1UL << g_reply_port->mp_SigBit;
        ULONG busySig  = g_busy_port ? (1UL << g_busy_port->mp_SigBit) : 0;

        for (;;) {
            /* Packets first: some may have queued up (signal already
             * consumed by the packet loop) before we got here. */
            if (g_busy_hook) g_busy_hook();
            if (GetMsg(g_reply_port)) break;
            Wait(replySig | busySig);
        }
        diag_log("backend_send: reply received\n");
    }
    if (g_req.server_now > g_backend_now) g_backend_now = g_req.server_now;
    if (g_req.cache_ttl > 0) g_backend_cache_ttl = g_req.cache_ttl;

    diag_log("backend_send: returning result=");
    diag_logn("", (LONG)g_req.result);
    if (reply) *reply = &g_req;
    return g_req.result;
}
