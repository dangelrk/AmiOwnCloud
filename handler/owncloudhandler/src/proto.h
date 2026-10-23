/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef OWNCLOUDHANDLER_PROTO_H
#define OWNCLOUDHANDLER_PROTO_H

/*
 * proto.h -- handler <-> worker wire protocol. Forked unchanged from
 * handler/cloudhandler/src/proto.h (see ../../cloudhandler/README.md for
 * the two-process design this follows) -- this protocol is already
 * backend-agnostic (WREQ_LIST/GET/PUT/... say nothing about AmiCloud vs.
 * WebDAV), only the port name below differs so an owncloud-handler process
 * can never accidentally talk to a running amicloud-worker or vice versa.
 * Included by both sides of the two-process split: handler.c/backend.c
 * (exec.library only, no libc, see handler.c's file header) and worker.c
 * (a normal vbcc-startup program linking libcloudhttp's webdavcmd.c and
 * friends). Kept deliberately standalone -- no vol.h, no libcloudcore/
 * libcloudhttp headers -- since the two sides have very different
 * available headers.
 *
 * One request at a time: the handler sends one WorkerMsg and waits for
 * the reply, reusing a single static request struct + a private reply
 * port (see backend.c). While it waits it still answers every packet
 * that needs no network (Info, Read/Write/Seek on open files, Examine
 * of a fresh directory, ...) and queues the rest -- see handler.c's
 * serve_while_busy(). How long a request may take is up to the worker's
 * HTTP layer: idle timeouts only (config timeout=), so a large file over
 * a modem takes as long as it needs while a dead line still fails.
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h> /* struct DateStamp */

/* Well-known public port name the worker registers (AddPort) and the
 * handler looks for (FindPort) during ACTION_STARTUP -- see backend.c. */
#define AC_WORKER_PORT "OwnCloud-Worker"

typedef enum {
    WREQ_PING = 0,
    WREQ_LIST,
    WREQ_GET,
    WREQ_PUT,
    WREQ_DEL,
    WREQ_MKDIR,
    WREQ_RENAME,
    WREQ_INFO,
    WREQ_SETCOMMENT,
    WREQ_SETPROTECT,
    WREQ_SETDATE,
    WREQ_QUIT,
    WREQ_READ        /* bytes offset..offset+buflen-1 of path into buf */
} WReqCmd;

#define WPATH_MAX 512

/* One directory entry as returned by WREQ_LIST. Deliberately NOT
 * core/libcloudcore's entry.h AcEntry (that one's shaped for the PC/CLI/shell display
 * layer) -- this is the wire format between handler and worker, sized to
 * match cloudhandler/vol.h's CNode fields it gets copied into. */
typedef struct {
    char name[107];      /* CNODE_MAXNAME+1, see vol.h */
    int  is_dir;
    LONG size;
    struct DateStamp mtime; /* already converted from the server's UTC
                              * epoch using the config's gmt_offset/dst --
                              * the worker does this (it has the config,
                              * the handler's packet loop never can, see
                              * worker.c epoch_to_datestamp(), mirroring
                              * core/libcloudfs/src/localfs_amiga.c). Ready to copy
                              * straight into a CNode/fib_Date. */
    LONG protection;       /* raw Amiga bits */
    char comment[80];
    char md5[33];
} WEntry;

/*
 * One request/reply. Embeds a plain exec Message (must be first member --
 * PutMsg()/GetMsg()/ReplyMsg() all operate on that), so a WorkerMsg* can
 * be sent and received exactly like any other exec message.
 *
 * Buffer ownership crosses a process boundary, so it is always AllocVec/
 * FreeVec, with exactly one owner per direction (docs/handler-plan.md
 * Stufe 2):
 *   WREQ_PUT:  handler AllocVec's buf/buflen (the write-buffered file),
 *              worker only reads it; handler FreeVec's it after the
 *              reply.
 *   WREQ_GET:  worker AllocVec's buf/buflen (the downloaded file
 *              content); handler FreeVec's it after copying/using it.
 *   WREQ_READ: same as WREQ_GET, just one byte range of the file.
 *   WREQ_LIST: worker AllocVec's the entries array; handler FreeVec's it
 *              after merging it into the cache.
 */
typedef struct WorkerMsg {
    struct Message msg;

    WReqCmd cmd;
    char    path[WPATH_MAX];
    char    path2[WPATH_MAX];  /* WREQ_RENAME target; unused otherwise */

    UBYTE  *buf;
    ULONG   buflen;
    LONG    offset;             /* in: WREQ_READ -- buflen is the length
                                  * wanted on the way in, the length got
                                  * on the way out (less at end of file) */

    struct DateStamp mtime;     /* in: WREQ_SETDATE; out: WREQ_INFO -- see
                                  * WEntry.mtime, same worker-side
                                  * conversion applies both directions */
    LONG    protection;         /* in: WREQ_SETPROTECT; out: WREQ_INFO */
    char    comment[80];        /* in: WREQ_SETCOMMENT; out: WREQ_INFO */

    WEntry *entries;             /* out: WREQ_LIST */
    LONG    n_entries;           /* out: WREQ_LIST */
    LONG    server_now;          /* out: every reply -- worker's clock at
                                   * the time of the reply, UTC-ish epoch
                                   * (see worker.c). Used as the cache_time
                                   * base by vol.c since the handler's
                                   * packet loop has no clock access of its
                                   * own (exec.library only). */
    LONG    cache_ttl;           /* out: every reply -- directory cache
                                   * lifetime in seconds from the config's
                                   * cache_ttl= (the handler can't read
                                   * files itself) */

    LONG    result;               /* out: finished DOS error code, 0 = ok */
} WorkerMsg;

#endif /* OWNCLOUDHANDLER_PROTO_H */
