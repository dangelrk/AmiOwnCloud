/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * handler.c -- OWNCLOUD: DOS handler entry point, ACTION_STARTUP and the
 * packet dispatch loop. Forked unchanged (see ../../cloudhandler/README.md
 * for the design this follows) from handler/cloudhandler/src/handler.c --
 * only the mounted-as device (OWNCLOUD:, via mount/OWNCLOUD) and the
 * registered Workbench volume name (register_volume() below) differ.
 * vol.c's tree is a cache of server state, reached through amiowncloud-worker
 * (worker.c, a separate process -- backend_init() below locates or spawns
 * it during startup).
 *
 * Two-process discipline (learned from a Guru 0x87000004 crash in an
 * earlier, deleted amicloud-handler prototype -- see
 * ../../cloudhandler/README.md): this process's
 * pr_MsgPort is BOTH the DOS packet port DOS delivers requests to AND,
 * transitively, something dos.library itself may need to coordinate
 * through while we're inside a dos.library call. Calling back into
 * dos.library while servicing our own packet queue can deadlock or corrupt
 * that port. So: dos.library is only ever touched during straight-line
 * startup code below (single call stack, before we've replied the startup
 * packet or touched our own port a second time) -- never from
 * dispatch()/act_*() in the packet loop, which is exec.library only,
 * replying packets by raw PutMsg() and never ReplyPkt()/DoPkt()/Delay().
 *
 * Linked -nostdlib with this file's object first (see Makefile.amiga-cross):
 * _start() below is the literal first byte of the loaded executable --
 * AmigaOS jumps straight into it for a GlobVec=-1 C/assembler handler
 * (RKRM DOS 11.5.4), there is no crt0 and no incoming register convention
 * to rely on, so the startup packet is always fetched via WaitPort/GetMsg
 * on our own process port, never assumed to arrive in a register.
 *
 * CRITICAL: "this file's object first on the link line" only guarantees
 * handler.o's CODE hunk comes first among the *files* -- it says nothing
 * about order *within* that hunk. AmigaOS jumps to literal offset 0 of
 * the hunk, not to a symbol named _start. The compiler emits function
 * bodies in the order they appear in the source, so _start() MUST be the
 * first function actually defined in this file (forward-declare anything
 * it calls, define those afterwards) -- verified the hard way: with
 * _start() declared last (after reply_pkt()/register_volume()/dispatch()),
 * a real link map showed __start sitting at hunk offset 0x3b0, not 0,
 * meaning AmigaOS was jumping straight into the middle of reply_pkt()'s
 * compiled code on every mount, with none of _start()'s own setup (SysBase,
 * __saveds global-data register, ...) ever having run -- explains a full
 * system freeze (corrupted exception vectors, garbage registers) on the
 * very first packet, independent of stack size or anything worker/network-
 * side, since nothing past word one of this file ever executed correctly.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "util.h"
#include "vol.h"
#include "actions.h"
#include "backend.h"

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;

const char amiga_ver[] = "$VER: amiowncloud-handler 0.1 (27.09.2026)";

/* Forward declarations only -- see the file header comment on why _start()
 * itself must be the first function DEFINED below, ahead of all of these. */
static void reply_pkt(struct DosPacket *pkt, LONG res1, LONG res2);
static void register_volume(struct Process *proc);
static void dispatch(struct DosPacket *pkt);
static void serve_while_busy(void);

/* Packets that arrived while a request to the worker was running and
 * need the network themselves -- answered in arrival order as soon as
 * the packet loop is free again (see serve_while_busy()). Linked through
 * each packet's own exec Message node, which we own between GetMsg()
 * and the reply. */
static struct List g_deferred;

void __saveds _start(void)
{
    struct Process   *proc;
    struct Message   *msg;
    struct DosPacket *pkt;
    struct DeviceNode *dn;

    SysBase = *(struct ExecBase **)4L;
    proc = (struct Process *)FindTask((CONST_STRPTR)0);

    WaitPort(&proc->pr_MsgPort);
    msg = GetMsg(&proc->pr_MsgPort);
    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

    dn = (struct DeviceNode *)BADDR(pkt->dp_Arg3);

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (!DOSBase) {
        reply_pkt(pkt, DOSFALSE, ERROR_INVALID_RESIDENT_LIBRARY);
        RemTask((struct Task *)0);
        return;
    }

    DateStamp(&g_boot_date);

    if (!vol_init(&g_boot_date)) {
        reply_pkt(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
        CloseLibrary((struct Library *)DOSBase);
        RemTask((struct Task *)0);
        return;
    }

    /* Locate or spawn amiowncloud-worker -- must happen before the startup
     * packet is replied (unlike register_volume() below, this doesn't
     * touch the device list, so there's no deadlock risk in doing it
     * here). No worker, no point mounting -- fail cleanly. */
    if (!backend_init()) {
        reply_pkt(pkt, DOSFALSE, ERROR_DEVICE_NOT_MOUNTED);
        CloseLibrary((struct Library *)DOSBase);
        RemTask((struct Task *)0);
        return;
    }

    /* Routes packets sent to "OWNCLOUD:" (the device we were mounted as)
     * to this process from now on. */
    dn->dn_Task = &proc->pr_MsgPort;
    g_my_port = &proc->pr_MsgPort;

    reply_pkt(pkt, DOSTRUE, 0);

    /* Only safe after the reply above -- see register_volume()'s comment. */
    register_volume(proc);

    /* Strictly exec.library only from here on (file header). */
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = (struct DosLibrary *)0;

    g_deferred.lh_Head     = (struct Node *)&g_deferred.lh_Tail;
    g_deferred.lh_Tail     = (struct Node *)0;
    g_deferred.lh_TailPred = (struct Node *)&g_deferred.lh_Head;
    backend_set_busy_hook(&proc->pr_MsgPort, serve_while_busy);

    for (;;) {
        /* Deferred packets are older than anything still in the port. */
        msg = (struct Message *)RemHead(&g_deferred);
        if (!msg) {
            WaitPort(&proc->pr_MsgPort);
            msg = GetMsg(&proc->pr_MsgPort);
            if (!msg) continue;
        }
        pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        dispatch(pkt);
        PutMsg(pkt->dp_Port, pkt->dp_Link);
    }
}

/* Packets that can be answered from memory alone, without a worker round
 * trip and without creating or freeing tree nodes -- safe to run while
 * another packet's request is still out at the worker (that one holds
 * CNode pointers; only network actions ever free nodes, and those wait).
 * 1 = always, 2 = only if no directory on its way needs refreshing (tried
 * in vol offline mode, queued if it would block), 0 = never. Lock/Examine
 * matter most: Info, List and Workbench all Lock() first. */
static int answerable_while_busy(struct DosPacket *pkt)
{
    switch (pkt->dp_Type) {
    case ACTION_IS_FILESYSTEM:
    case ACTION_DISK_INFO:
    case ACTION_INFO:
    case ACTION_CURRENT_VOLUME:
    case ACTION_FLUSH:
    case ACTION_SEEK:
    case ACTION_EXAMINE_FH:
    case ACTION_SAME_LOCK:
    case ACTION_FREE_LOCK:
        return 1;
    case ACTION_READ:          /* may need a range from the server */
    case ACTION_WRITE:         /* may need the whole file first */
    case ACTION_SET_FILE_SIZE:
    case ACTION_LOCATE_OBJECT:
    case ACTION_EXAMINE_OBJECT:
    case ACTION_EXAMINE_NEXT:
    case ACTION_PARENT:
    case ACTION_PARENT_FH:
    case ACTION_COPY_DIR:
    case ACTION_COPY_DIR_FH:
        return 2;
    default:
        return 0;
    }
}

/* backend_send()'s busy hook (see backend.h): drains our packet port
 * while the worker is busy, answering what needs no network right away
 * and queueing the rest for the packet loop. One slow download no longer
 * freezes Info, Workbench's disk gauge or reading other open files. */
static void serve_while_busy(void)
{
    struct Message *msg;

    while ((msg = GetMsg(g_my_port)) != (struct Message *)0) {
        struct DosPacket *pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        int how = answerable_while_busy(pkt);
        int done = 0;

        if (how == 1) {
            dispatch(pkt);
            done = 1;
        } else if (how == 2) {
            vol_set_offline(1);
            dispatch(pkt);
            vol_set_offline(0);
            done = !vol_would_block();
        }
        if (done) PutMsg(pkt->dp_Port, pkt->dp_Link);
        else      AddTail(&g_deferred, &msg->mn_Node);
    }
}

static void reply_pkt(struct DosPacket *pkt, LONG res1, LONG res2)
{
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(pkt->dp_Port, pkt->dp_Link);
}

/* Registers the "OwnCloud" volume (Workbench disk icon) alongside the
 * "OWNCLOUD" device we were mounted as -- both dol_Task point at our port,
 * so either name reaches us. Must run AFTER the startup packet has been
 * replied: GetDeviceProc() holds the device list locked for the whole
 * startup handshake (RKRM DOS 13.1.2), so calling AddDosEntry() -- which
 * itself locks the list (RKRM DOS 8.4.1) -- any earlier would deadlock
 * against ourselves. Non-fatal on failure: OWNCLOUD: still works, just
 * without a Workbench icon. */
static void register_volume(struct Process *proc)
{
    struct DosList *voldl;
    int tries;

    voldl = MakeDosEntry((CONST_STRPTR)"OwnCloud", DLT_VOLUME);
    if (!voldl) return;

    u_memcpy(&voldl->dol_misc.dol_volume.dol_VolumeDate, &g_boot_date, sizeof(struct DateStamp));
    voldl->dol_misc.dol_volume.dol_LockList = 0;
    voldl->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
    voldl->dol_Task = &proc->pr_MsgPort;

    for (tries = 0; tries < 10; tries++) {
        if (AttemptLockDosList(LDF_VOLUMES | LDF_WRITE)) {
            LONG ok = AddDosEntry(voldl);
            UnLockDosList(LDF_VOLUMES | LDF_WRITE);
            if (ok) { g_volume_bptr = MKBADDR(voldl); return; }
            break; /* name collision or similar -- give up, non-fatal */
        }
        Delay(1);
    }
    FreeDosEntry(voldl);
}

static void dispatch(struct DosPacket *pkt)
{
    switch (pkt->dp_Type) {
    case ACTION_IS_FILESYSTEM:  act_is_filesystem(pkt);  break;
    case ACTION_LOCATE_OBJECT:  act_locate_object(pkt);  break;
    case ACTION_FREE_LOCK:      act_free_lock(pkt);      break;
    case ACTION_COPY_DIR:       act_copy_dir(pkt);       break;
    case ACTION_COPY_DIR_FH:    act_copy_dir_fh(pkt);    break;
    case ACTION_PARENT:         act_parent(pkt);         break;
    case ACTION_PARENT_FH:      act_parent_fh(pkt);      break;
    case ACTION_SAME_LOCK:      act_same_lock(pkt);      break;
    case ACTION_CHANGE_MODE:    act_change_mode(pkt);    break;
    case ACTION_CREATE_DIR:     act_create_dir(pkt);     break;

    case ACTION_EXAMINE_OBJECT: act_examine_object(pkt); break;
    case ACTION_EXAMINE_NEXT:   act_examine_next(pkt);   break;
    case ACTION_EXAMINE_FH:     act_examine_fh(pkt);     break;

    case ACTION_FINDINPUT:      act_findinput(pkt);      break;
    case ACTION_FINDOUTPUT:     act_findoutput(pkt);     break;
    case ACTION_FINDUPDATE:     act_findupdate(pkt);     break;
    case ACTION_FH_FROM_LOCK:   act_fh_from_lock(pkt);   break;
    case ACTION_END:            act_end(pkt);            break;
    case ACTION_READ:           act_read(pkt);           break;
    case ACTION_WRITE:          act_write(pkt);          break;
    case ACTION_SEEK:           act_seek(pkt);           break;
    case ACTION_SET_FILE_SIZE:  act_set_file_size(pkt);  break;

    case ACTION_DELETE_OBJECT:  act_delete_object(pkt);  break;
    case ACTION_RENAME_OBJECT:  act_rename_object(pkt);  break;
    case ACTION_SET_PROTECT:    act_set_protect(pkt);    break;
    case ACTION_SET_COMMENT:    act_set_comment(pkt);    break;
    case ACTION_SET_DATE:       act_set_date(pkt);       break;

    case ACTION_DISK_INFO:      act_disk_info(pkt);      break;
    case ACTION_INFO:           act_info(pkt);           break;
    case ACTION_CURRENT_VOLUME: act_current_volume(pkt); break;
    case ACTION_FLUSH:          act_flush(pkt);          break;

    case ACTION_DIE:
        /* Unmounting needs dos.library (AttemptLockDosList/RemDosEntry),
         * which is closed by the time we're servicing packets -- see the
         * file header. Always refuse; see README "Bekannte Einschraenkungen". */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_OBJECT_IN_USE;
        break;

    default:
        /* Includes ACTION_EXAMINE_ALL -- unimplemented on purpose, DOS
         * falls back to EXAMINE_OBJECT/EXAMINE_NEXT (docs/handler-plan.md). */
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
        break;
    }
}
