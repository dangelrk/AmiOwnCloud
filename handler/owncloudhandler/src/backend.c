/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "backend.h"
#include "util.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/timer.h>
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

/* DISABLED -- root-caused to timer.device itself, not a suspect anymore
 * for the mount/list crash (that was _start() not being at hunk offset 0,
 * see handler.c's file header, and the embedded-colon path-parsing gap in
 * resolve_walk(), see actions.c -- both fixed and confirmed separately).
 *
 * The bounded-wait mechanism below was meant as a last-resort safety net
 * against a wedged worker, but turned out to BE the bug: every second
 * backend_send() call within a mount session (the first WREQ_GET after a
 * successful WREQ_LIST, tested repeatedly) would time out even though the
 * worker's own log proved ReplyMsg() ran successfully within a couple of
 * seconds. Diagnostic instrumentation (diag_log() below, plus logging
 * Wait()'s actual returned signal mask) proved this wasn't a lost message
 * or a stale pointer: mn_ReplyPort matched exactly on both sides, and
 * sizeof(WorkerMsg)/field offsets matched exactly between the two
 * differently-flagged compilation units (ruling out an ABI mismatch too).
 * Wait(replySig|timerSig) on the second call returned with ONLY timerSig
 * set -- the reply signal never fired at all, despite the identical code
 * path working perfectly on the first call.
 *
 * First fix attempt: stopped reusing one static struct timerequest across
 * calls (CreateIORequest()/OpenDevice() fresh every backend_send() call
 * instead) -- g_timer_port itself stayed persistent. This did NOT fix it;
 * identical failure, proven by testing with a fresh IORequest every call.
 *
 * Actual fix: disabling timer.device entirely (this flag) -- falling back
 * to a plain unbounded WaitPort()/GetMsg() in backend_send() -- immediately
 * fixed it, confirmed via testing (Copy + lha extraction of a real file
 * both worked cleanly). So the persistent g_timer_port itself -- reused
 * for multiple OpenDevice()/CloseDevice() cycles across calls -- is
 * implicated, not the per-call IORequest. Plausibly an AmiBerry
 * timer.device/UNIT_VBLANK emulation quirk rather than a genuine AmigaOS
 * API misuse, but not worth re-risking without a redesigned mechanism
 * (e.g. a watcher task, or a timer port opened once and never closed/
 * reopened for the rest of the mount) and real hardware testing. Until
 * then: no bounded timeout, matching this project's pre-existing documented
 * failure model (README, Stage 2 test 8: a wedged worker needs a fresh
 * Mount to recover from, not a live-restart) -- just without the ~60s
 * detection window this was meant to add. */
#define AC_BACKEND_TIMER_ENABLED 0

static struct MsgPort     *g_timer_port;
static int                  g_timer_ok;

/* Generous on purpose: WREQ_GET/WREQ_PUT can legitimately take a while for
 * a large file over a slow link, and a false-positive timeout here is
 * worse than a slow-but-eventually-successful request (it permanently
 * kills the backend for the rest of this mount, see g_backend_dead
 * below). This is a last-resort safety net for a genuinely wedged worker,
 * not a snappy per-request budget. */
#define BACKEND_TIMEOUT_SECS 60

/* Set once a request has timed out and never cleared. From that point on
 * g_req may be written to by the worker at ANY later, unpredictable time
 * (it never learned we gave up on it) -- reusing/resetting g_req for a
 * new request while that's still possible would be a real cross-process
 * memory race (both processes share one flat address space, see proto.h).
 * So instead: once dead, stay dead for the rest of this mount and fail
 * every further backend_send() immediately and cleanly. This matches the
 * project's already-documented failure model (README, Stage 2 test 8: a
 * worker that dies/wedges mid-session needs a fresh Mount to recover from,
 * not a live-restart) -- the only change is a bounded ~60s detection time
 * instead of an unbounded hang. */
static int g_backend_dead;

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

#if AC_BACKEND_TIMER_ENABLED
    /* Just verify timer.device is actually openable and keep the port for
     * reuse (see g_timer_port's comment above) -- the per-call IORequest
     * itself is created/opened/closed fresh in backend_send() every time,
     * not here. Non-fatal if this fails (exotic -- timer.device is always
     * present on a real/emulated Amiga): g_timer_ok just stays 0 and
     * backend_send() falls back to the old unbounded WaitPort(). */
    g_timer_port = CreateMsgPort();
    if (g_timer_port) {
        struct timerequest *probe = (struct timerequest *)CreateIORequest(g_timer_port, sizeof(struct timerequest));
        if (probe) {
            if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)probe, 0) == 0) {
                g_timer_ok = 1;
                CloseDevice((struct IORequest *)probe);
            }
            DeleteIORequest((APTR)probe);
        }
        if (!g_timer_ok) { DeleteMsgPort(g_timer_port); g_timer_port = NULL; }
    }
#endif /* AC_BACKEND_TIMER_ENABLED */

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
    started = SystemTags((CONST_STRPTR)"L:owncloud-worker",
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

LONG backend_send(WReqCmd cmd, const char *path, const char *path2,
                  UBYTE *buf, ULONG buflen,
                  const struct DateStamp *mtime, LONG protection, const char *comment,
                  WorkerMsg **reply)
{
    if (g_backend_dead) {
        if (reply) *reply = (WorkerMsg *)0;
        return ERROR_LOCK_TIMEOUT;
    }

    u_memset(&g_req, 0, sizeof(g_req));
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
     * stubbed (that's all a plain "Dir CLOUD:" needs); anything else just
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

#if AC_BACKEND_TIMER_ENABLED
    {
        /* Fresh IORequest every call -- see g_timer_port's comment above
         * for why this must NOT be a reused static like g_timer_port
         * itself is. NULL-safe throughout: if any setup step fails, tio
         * stays NULL and we just fall through to the old unbounded wait. */
        struct timerequest *tio = (struct timerequest *)0;

        if (g_timer_ok) {
            tio = (struct timerequest *)CreateIORequest(g_timer_port, sizeof(struct timerequest));
            if (tio && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)tio, 0) != 0) {
                DeleteIORequest((APTR)tio);
                tio = (struct timerequest *)0;
            }
        }

        if (!tio) {
            /* No timer.device this time -- old unbounded wait. */
            diag_log("backend_send: no timer, WaitPort...\n");
            WaitPort(g_reply_port);
            GetMsg(g_reply_port);
            diag_log("backend_send: WaitPort+GetMsg done (no timer)\n");
        } else {
            ULONG replySig = 1UL << g_reply_port->mp_SigBit;
            ULONG timerSig = 1UL << g_timer_port->mp_SigBit;

            tio->tr_node.io_Command = TR_ADDREQUEST;
            tio->tr_time.tv_secs    = BACKEND_TIMEOUT_SECS;
            tio->tr_time.tv_micro   = 0;
            diag_log("backend_send: SendIO(timer)...\n");
            SendIO((struct IORequest *)tio);

            diag_log("backend_send: mn_ReplyPort=");
            diag_logn("", (LONG)g_req.msg.mn_ReplyPort);
            diag_log("backend_send: Wait(replySig|timerSig)...\n");
            {
                ULONG got = Wait(replySig | timerSig);
                diag_log("backend_send: Wait() returned, got=");
                diag_logn("", (LONG)got);
                diag_log("backend_send: replySig=");
                diag_logn("", (LONG)replySig);
                diag_log("backend_send: timerSig=");
                diag_logn("", (LONG)timerSig);
            }

            if (GetMsg(g_reply_port)) {
                /* Real reply arrived -- cancel+retire the still-pending
                 * timer request (AbortIO() on an IORequest that has
                 * already finished on its own is documented-safe, so this
                 * is correct even in the race where both fire at almost
                 * the same moment), then tear the whole IORequest down --
                 * it is NEVER reused for a later call. */
                diag_log("backend_send: GetMsg found reply, AbortIO(timer)...\n");
                AbortIO((struct IORequest *)tio);
                diag_log("backend_send: AbortIO done, WaitIO(timer)...\n");
                WaitIO((struct IORequest *)tio);
                diag_log("backend_send: WaitIO(timer) done, closing device\n");
                CloseDevice((struct IORequest *)tio);
                DeleteIORequest((APTR)tio);
            } else {
                /* Timer fired first -- the worker is still out there
                 * somewhere holding a pointer to g_req. Retire+tear down
                 * the now-completed timer request and poison the backend
                 * for good (see g_backend_dead's comment) rather than
                 * ever touch g_req again. */
                diag_log("backend_send: GetMsg found NOTHING, WaitIO(timer)...\n");
                WaitIO((struct IORequest *)tio);
                diag_log("backend_send: WaitIO(timer) done, closing device, poisoning backend, returning TIMEOUT\n");
                CloseDevice((struct IORequest *)tio);
                DeleteIORequest((APTR)tio);
                g_backend_dead = 1;
                if (reply) *reply = (WorkerMsg *)0;
                return ERROR_LOCK_TIMEOUT;
            }
        }
    }
#else
    /* Timer disabled for crash isolation (see AC_BACKEND_TIMER_ENABLED
     * above) -- exactly the original, pre-timeout unbounded wait. */
    diag_log("backend_send: [no-timer build] WaitPort...\n");
    WaitPort(g_reply_port);
    diag_log("backend_send: [no-timer build] WaitPort returned, GetMsg...\n");
    if (!GetMsg(g_reply_port)) {
        diag_log("backend_send: [no-timer build] GetMsg found NOTHING after WaitPort!\n");
    } else {
        diag_log("backend_send: [no-timer build] GetMsg found reply\n");
    }
#endif /* AC_BACKEND_TIMER_ENABLED */

    diag_log("backend_send: returning result=");
    diag_logn("", (LONG)g_req.result);
    if (reply) *reply = &g_req;
    return g_req.result;
}
