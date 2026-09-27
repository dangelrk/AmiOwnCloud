/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef OWNCLOUDHANDLER_ACTIONS_H
#define OWNCLOUDHANDLER_ACTIONS_H

/*
 * actions.h -- one function per ACTION_* packet type. handler.c's packet
 * loop dispatches dp_Type to these; each function reads its own dp_ArgN
 * and fills dp_Res1/dp_Res2 itself (does not reply the packet -- handler.c
 * does that uniformly via raw PutMsg once control returns here).
 *
 * Stage 2 (see docs/handler-plan.md): vol.c's tree became a cache of
 * server state and most of these now go through backend_send() to reach
 * the worker process for the actual server round trip; SET_PROTECT/
 * SET_COMMENT/SET_DATE still only touch the local cache (wired to the
 * server in Stage 3).
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

/* Set once during ACTION_STARTUP (handler.c) and used read-only from here
 * on: our own process port (for fl_Task/dol_Task), the BPTR to the
 * self-created DLT_VOLUME DosList entry (for fl_Volume, ACTION_CURRENT_
 * VOLUME, id_VolumeNode), and a boot-time snapshot used as the mtime for
 * newly created nodes (no clock access is possible from the packet loop,
 * see docs/handler-plan.md "Zwei-Prozess-Lektionen"). */
extern struct MsgPort   *g_my_port;
extern BPTR               g_volume_bptr;
extern struct DateStamp   g_boot_date;

/* Identifies an open file; stored in fh_Arg1 of the FileHandle DOS gave us
 * (ACTION_FINDINPUT/OUTPUT/UPDATE, ACTION_FH_FROM_LOCK).
 *
 * Stage 2: the file content buffer lives HERE, per open handle, not on
 * the shared CNode anymore (docs/handler-plan.md Stufe 2 "Lesen"/
 * "Schreiben"). FINDINPUT/FINDUPDATE/FH_FROM_LOCK download the whole
 * object into data/size/alloc via WREQ_GET when opened; WRITE/
 * SET_FILE_SIZE grow and mutate that same buffer and set dirty; END
 * uploads it via WREQ_PUT iff dirty. This also means concurrent readers
 * of the same file each get their own independent snapshot from open
 * time -- deliberate, avoids any cross-handle cache-coherency problem for
 * file bodies (only directory listings are actually cached, see vol.h). */
typedef struct OpenFile {
    struct CNode *node;
    LONG          pos;
    int           exclusive; /* 1 if this handle grants exclusive access
                               * (ACTION_FINDOUTPUT, or absorbed from an
                               * EXCLUSIVE_LOCK via ACTION_FH_FROM_LOCK) --
                               * mirrored into node->excl_lock while open. */
    int           dirty;      /* data/size holds unpushed local changes */
    UBYTE        *data;        /* AllocVec'd, NULL if never grown/downloaded */
    LONG          size;         /* bytes valid in *data */
    LONG          alloc;         /* bytes allocated at *data */
} OpenFile;

/* Locks and directory lookups */
void act_locate_object(struct DosPacket *pkt);
void act_free_lock(struct DosPacket *pkt);
void act_copy_dir(struct DosPacket *pkt);
void act_copy_dir_fh(struct DosPacket *pkt);
void act_parent(struct DosPacket *pkt);
void act_parent_fh(struct DosPacket *pkt);
void act_same_lock(struct DosPacket *pkt);
void act_change_mode(struct DosPacket *pkt);
void act_create_dir(struct DosPacket *pkt);

/* Examine / directory scanning */
void act_examine_object(struct DosPacket *pkt);
void act_examine_next(struct DosPacket *pkt);
void act_examine_fh(struct DosPacket *pkt);

/* File I/O */
void act_findinput(struct DosPacket *pkt);
void act_findoutput(struct DosPacket *pkt);
void act_findupdate(struct DosPacket *pkt);
void act_fh_from_lock(struct DosPacket *pkt);
void act_end(struct DosPacket *pkt);
void act_read(struct DosPacket *pkt);
void act_write(struct DosPacket *pkt);
void act_seek(struct DosPacket *pkt);
void act_set_file_size(struct DosPacket *pkt);

/* Metadata / tree mutation */
void act_delete_object(struct DosPacket *pkt);
void act_rename_object(struct DosPacket *pkt);
void act_set_protect(struct DosPacket *pkt);
void act_set_comment(struct DosPacket *pkt);
void act_set_date(struct DosPacket *pkt);

/* Volume-level */
void act_info(struct DosPacket *pkt);
void act_disk_info(struct DosPacket *pkt);
void act_current_volume(struct DosPacket *pkt);
void act_is_filesystem(struct DosPacket *pkt);
void act_flush(struct DosPacket *pkt);

#endif /* OWNCLOUDHANDLER_ACTIONS_H */
