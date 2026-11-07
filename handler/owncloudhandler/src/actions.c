/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * actions.c -- one function per ACTION_* packet type. vol.c's tree is a
 * cache of server state (Stage 2); every mutating action here applies to
 * the local cache *and* reaches the server through backend_send() (see
 * backend.h) -- SET_PROTECT/SET_COMMENT/SET_DATE joined the rest in
 * Stage 3.
 *
 * Argument conventions below were checked against Include_H/dos/dosextens.h
 * (ACTION_* values, struct DosPacket/FileLock/FileHandle layout) and
 * rkrm/rkrm-dos.pdf chapter 14 ("Packets"), notably:
 *   - 14.2.1 ACTION_LOCATE_OBJECT, 14.2.2 COPY_DIR, 14.2.3 PARENT,
 *     14.2.4 COPY_DIR_FH, 14.2.5 PARENT_FH, 14.2.6 CREATE_DIR,
 *     14.2.7 SAME_LOCK, 14.2.8 CHANGE_MODE, 14.2.9 FREE_LOCK
 *   - 14.3.1 EXAMINE_OBJECT, 14.3.2 EXAMINE_NEXT, 14.3.5 EXAMINE_FH
 *     (fib_FileName/fib_Comment are BSTRs at the packet level; converted
 *     to C strings by dos.library for the caller -- see util.c
 *     fill_bstr_field(); fib_DiskKey must be a stable, non-reused per-
 *     directory sequence number, never a raw pointer)
 *   - 14.1.1-14.1.9 FINDINPUT/FINDOUTPUT/FINDUPDATE/END/READ/WRITE/SEEK/
 *     SET_FILE_SIZE, 14.1.4 FH_FROM_LOCK
 *   - 14.5.1 RENAME_OBJECT, 14.5.2 DELETE_OBJECT, 14.5.3 SET_PROTECT,
 *     14.5.4 SET_COMMENT, 14.5.5 SET_DATE -- note SET_PROTECT/SET_COMMENT/
 *     SET_DATE take their lock in dp_Arg2, NOT dp_Arg1 (dp_Arg1 is unused
 *     for these three; easy to get wrong, see RKRM footnotes there)
 *   - 14.7.1 CURRENT_VOLUME, 14.7.2 INFO, 14.7.3 DISK_INFO
 *   - 14.9.3 IS_FILESYSTEM, 14.9.4 FLUSH
 */

#include "actions.h"
#include "vol.h"
#include "util.h"
#include "backend.h"

#include <exec/memory.h>
#include <proto/exec.h>

#define AC_PATH_BUF 512

struct MsgPort   *g_my_port;
BPTR               g_volume_bptr;
struct DateStamp   g_boot_date;

/* Crash-hunt diagnostic (temporary, see vol.h/backend.h): a standalone
 * CNode for the synthetic "debuglog" file, deliberately NOT linked into
 * g_root->children. act_locate_object() and do_find() both intercept the
 * raw path string for this ONE special case before any resolve()/
 * vol_find_child() tree lookup -- which would itself need a working
 * backend_send()/vol_refresh() round trip, exactly the thing this file
 * exists to help diagnose after that's stopped working. */
static CNode g_diaglog_node;

static void diaglog_ensure_init(void)
{
    if (g_diaglog_node.name[0] == '\0') {
        u_strncpy(g_diaglog_node.name, "debuglog", sizeof(g_diaglog_node.name));
        g_diaglog_node.type = CN_FILE;
        g_diaglog_node.is_diaglog = 1;
        u_memcpy(&g_diaglog_node.date, &g_boot_date, sizeof(struct DateStamp));
    }
    /* Always refreshed, not just on first init -- g_diag_pos keeps growing
     * as more backend_send() calls happen, and a plain Lock()+Examine()
     * (no Open()) must still report the current size. */
    g_diaglog_node.size = (LONG)g_diag_pos;
}

/* Is base/cpath a bare, root-level "debuglog" lookup? Shared by
 * act_locate_object() and do_find()'s try_open_diaglog(). */
static int is_diaglog_path(CNode *base, const char *cpath)
{
    return base == g_root && u_stricmp_latin1(cpath, "debuglog") == 0;
}

/* ---- shared path resolution ---------------------------------------- */

typedef struct {
    CNode *dir;                       /* parent directory (always valid) */
    char   leaf[CNODE_MAXNAME + 1];   /* "" if *dir* itself is the target */
    CNode *node;                      /* resolved object, or NULL if leaf
                                        * doesn't exist (only meaningful
                                        * when leaf[0] != 0) */
} ResolvedPath;

/* Walks cpath component by component starting at base (see util.h
 * path_next_component for the parent-marker/root rules). Only fails (sets
 * *err) if a non-final component doesn't exist or isn't a directory. */
static int resolve_walk(CNode *base, const char *cpath, CNode **out_dir, char *out_leaf, ULONG leafsz, LONG *err)
{
    CNode *cur = base;
    const char *p = cpath;
    char comp[CNODE_MAXNAME + 1];
    char pending[CNODE_MAXNAME + 1];
    int is_parent;
    int have_pending = 0;

    out_leaf[0] = '\0';

    /* AmigaDOS convention: an embedded colon anywhere in a path means
     * "reset to that device's root, continue from here" (paths can
     * legitimately contain e.g. "SYS:foo/bar" mid-string). Confirmed by
     * testing on the sibling amicloud-handler (crash-hunt diagnostic,
     * see ../../cloudhandler/src/actions.c): dos.library's Lock() for a
     * bare "OWNCLOUD:"/"OwnCloud:" request passes the matched device-
     * name-plus-colon straight through as the object path in dp_Arg2 --
     * NOT an empty string as RKRM DOS Table 14.12's summary of the
     * general case suggested. We're only ever reached for paths that
     * already resolved to OUR device/volume, so any colon just means
     * "our own root" -- there's no other device to distinguish here.
     * Without this, a colon anywhere in the path (most commonly the
     * whole "owncloud:"/"ownCloud:" string on its own) was being looked
     * up as a literal, nonexistent child filename of the root, failing
     * with ERROR_OBJECT_NOT_FOUND. */
    {
        const char *colon = (const char *)0;
        const char *scan;
        for (scan = p; *scan; scan++) if (*scan == ':') colon = scan;
        if (colon) { cur = g_root; p = colon + 1; }
    }

    while (path_next_component(&p, comp, sizeof(comp), &is_parent)) {
        if (have_pending) {
            CNode *next = vol_find_child(cur, pending);
            if (!next || next->type != CN_DIR) {
                *err = (!next && vol_lookup_error()) ? vol_lookup_error() : ERROR_OBJECT_NOT_FOUND;
                return 0;
            }
            cur = next;
            have_pending = 0;
        }
        if (is_parent) {
            if (cur->parent) cur = cur->parent;
            continue;
        }
        u_strncpy(pending, comp, sizeof(pending));
        have_pending = 1;
    }

    if (have_pending) u_strncpy(out_leaf, pending, leafsz);
    *out_dir = cur;
    return 1;
}

static LONG resolve(CNode *base, const char *cpath, ResolvedPath *rp)
{
    LONG err = 0;
    if (!resolve_walk(base, cpath, &rp->dir, rp->leaf, sizeof(rp->leaf), &err)) return err;
    if (rp->leaf[0] == '\0') { rp->node = rp->dir; return 0; }
    rp->node = vol_find_child(rp->dir, rp->leaf);
    /* Directory not listable (server unreachable): say so instead of
     * "not found" -- a Copy to OWNCLOUD: must not think it may create it. */
    if (!rp->node && vol_lookup_error()) return vol_lookup_error();
    return 0;
}

/* ---- FileInfoBlock filling ------------------------------------------- */

static void fill_fib(struct FileInfoBlock *fib, CNode *node)
{
    const char *nm = (node == g_root) ? "OwnCloud" : node->name;

    fib->fib_DiskKey = (LONG)node->seq;
    fib->fib_DirEntryType = (node->type == CN_DIR) ? ((node == g_root) ? ST_ROOT : ST_USERDIR) : ST_FILE;
    fib->fib_EntryType = fib->fib_DirEntryType;
    fill_bstr_field(fib->fib_FileName, sizeof(fib->fib_FileName), nm);
    fib->fib_Protection = node->protection;
    fib->fib_Size = (node->type == CN_DIR) ? 0 : node->size;
    fib->fib_NumBlocks = (fib->fib_Size + 511) / 512;
    u_memcpy(&fib->fib_Date, &node->date, sizeof(struct DateStamp));
    fill_bstr_field(fib->fib_Comment, sizeof(fib->fib_Comment), node->comment);
    fib->fib_OwnerUID = 0;
    fib->fib_OwnerGID = 0;
}

/* ---- locks and directory lookups -------------------------------------- */

void act_locate_object(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg1);
    char cpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;
    BPTR lock;

    bstr_to_cstr(pkt->dp_Arg2, cpath, sizeof(cpath));

    /* Crash-hunt diagnostic (temporary, see vol.h/backend.h/is_diaglog_
     * path()): intercepted here too, not just do_find() below -- Copy and
     * friends Lock() a file before Open()ing it, and the normal resolve()
     * path below would otherwise need a working vol_refresh() round trip
     * just to find out it doesn't exist as a real tree entry. */
    if (is_diaglog_path(base, cpath)) {
        diaglog_ensure_init();
        lock = vol_lock_create(&g_diaglog_node, pkt->dp_Arg3, g_my_port, g_volume_bptr, &err);
        if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
        pkt->dp_Res1 = lock;
        pkt->dp_Res2 = 0;
        return;
    }

    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    if (!rp.node) { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }

    lock = vol_lock_create(rp.node, pkt->dp_Arg3, g_my_port, g_volume_bptr, &err);
    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

void act_free_lock(struct DosPacket *pkt)
{
    vol_lock_release(pkt->dp_Arg1); /* no-op if ZERO, matches RKRM 14.2.9 */
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_copy_dir(struct DosPacket *pkt)
{
    CNode *node = vol_node_from_lock(pkt->dp_Arg1);
    LONG err;
    BPTR lock = vol_lock_create(node, SHARED_LOCK, g_my_port, g_volume_bptr, &err);

    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

void act_copy_dir_fh(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    LONG err;
    BPTR lock;

    if (of->exclusive) { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return; }
    lock = vol_lock_create(of->node, SHARED_LOCK, g_my_port, g_volume_bptr, &err);
    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

void act_parent(struct DosPacket *pkt)
{
    CNode *node = vol_node_from_lock(pkt->dp_Arg1);
    LONG err;
    BPTR lock;

    if (!node->parent) { pkt->dp_Res1 = 0; pkt->dp_Res2 = 0; return; } /* root: not a failure */
    lock = vol_lock_create(node->parent, SHARED_LOCK, g_my_port, g_volume_bptr, &err);
    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

void act_parent_fh(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    CNode *node = of->node;
    LONG err;
    BPTR lock;

    if (!node->parent) { pkt->dp_Res1 = 0; pkt->dp_Res2 = 0; return; }
    lock = vol_lock_create(node->parent, SHARED_LOCK, g_my_port, g_volume_bptr, &err);
    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

void act_same_lock(struct DosPacket *pkt)
{
    CNode *n1 = vol_node_from_lock(pkt->dp_Arg1);
    CNode *n2 = vol_node_from_lock(pkt->dp_Arg2);

    pkt->dp_Res1 = (n1 == n2) ? DOSTRUE : DOSFALSE;
    pkt->dp_Res2 = 0;
}

void act_change_mode(struct DosPacket *pkt)
{
    LONG kind = pkt->dp_Arg1;
    LONG target = pkt->dp_Arg3;
    int want_excl = (target == EXCLUSIVE_LOCK || target == ACTION_FINDOUTPUT);

    if (kind == CHANGE_LOCK) {
        struct FileLock *fl;
        CNode *node;

        if (!pkt->dp_Arg2) {
            /* ZERO lock == root sentinel (see vol_node_from_lock): there is
             * no real struct FileLock to mutate. Root can never go
             * exclusive; a shared request is trivially a no-op. */
            if (want_excl) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return; }
            pkt->dp_Res1 = DOSTRUE; pkt->dp_Res2 = 0; return;
        }

        fl = (struct FileLock *)BADDR(pkt->dp_Arg2);
        node = (CNode *)fl->fl_Key;

        if (want_excl) {
            if (node == g_root || node->lock_count != 1 || node->excl_lock || node->open_count != 0) {
                pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
            }
            node->lock_count = 0;
            node->excl_lock = 1;
            fl->fl_Access = EXCLUSIVE_LOCK;
        } else if (fl->fl_Access == EXCLUSIVE_LOCK) {
            node->excl_lock = 0;
            node->lock_count = 1;
            fl->fl_Access = SHARED_LOCK;
        }
        pkt->dp_Res1 = DOSTRUE; pkt->dp_Res2 = 0;
        return;
    }

    if (kind == CHANGE_FH) {
        struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg2);
        OpenFile *of = (OpenFile *)fh->fh_Arg1;
        CNode *node = of->node;

        if (want_excl) {
            if (node == g_root || node->open_count != 1 || node->excl_lock || node->lock_count != 0) {
                pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
            }
            node->excl_lock = 1;
            of->exclusive = 1;
        } else if (of->exclusive) {
            node->excl_lock = 0;
            of->exclusive = 0;
        }
        pkt->dp_Res1 = DOSTRUE; pkt->dp_Res2 = 0;
        return;
    }

    pkt->dp_Res1 = DOSFALSE;
    pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
}

void act_create_dir(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg1);
    char cpath[AC_PATH_BUF], fullpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;
    CNode *node;
    BPTR lock;

    bstr_to_cstr(pkt->dp_Arg2, cpath, sizeof(cpath));
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    if (rp.leaf[0] == '\0') { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
    if (rp.node) { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_OBJECT_EXISTS; return; }

    vol_child_path(rp.dir, rp.leaf, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_MKDIR, fullpath, (const char *)0, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }

    node = vol_new_node(rp.dir, rp.leaf, CN_DIR, &g_boot_date);
    if (!node) { pkt->dp_Res1 = 0; pkt->dp_Res2 = ERROR_NO_FREE_STORE; return; }
    vol_invalidate(rp.dir);

    lock = vol_lock_create(node, EXCLUSIVE_LOCK, g_my_port, g_volume_bptr, &err);
    if (!lock) { pkt->dp_Res1 = 0; pkt->dp_Res2 = err; return; }
    pkt->dp_Res1 = lock;
    pkt->dp_Res2 = 0;
}

/* ---- examine / directory scanning ------------------------------------ */

void act_examine_object(struct DosPacket *pkt)
{
    CNode *node = vol_node_from_lock(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);
    LONG err;

    err = vol_ensure_fresh(node); /* no-op for files; may (re)populate a directory's children */
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    fill_fib(fib, node);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* fib_DiskKey doubles as the ExNext cursor: node->seq is a global,
 * monotonically increasing, never-reused counter (vol_next_seq()), so
 * "smallest child seq greater than the cursor" is well defined regardless
 * of list order or concurrent modifications (RKRM DOS 14.3.2). */
void act_examine_next(struct DosPacket *pkt)
{
    CNode *dir = vol_node_from_lock(pkt->dp_Arg1);
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);
    ULONG cursor = (ULONG)fib->fib_DiskKey;
    CNode *best = (CNode *)0;
    CNode *c;
    LONG err;

    if (dir->type != CN_DIR) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_INVALID_LOCK; return; }
    /* vol_find_child() elsewhere does this implicitly; here we walk dir->children directly */
    err = vol_ensure_fresh(dir);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    /* The first ExNext after Examine(dir) carries the directory's own
     * key. That is "start from the top", not "after seq N": a child that
     * was renamed into this directory is older than it (smaller seq) and
     * used to be skipped -- a dir showed empty yet "Delete ALL" refused
     * it as not empty. Keys are unique and a dir is never its own child,
     * so this can't be mistaken for a real position. */
    if (cursor == dir->seq) cursor = 0;

    for (c = dir->children; c; c = c->next)
        if (c->seq > cursor && (!best || c->seq < best->seq)) best = c;

    if (!best) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_NO_MORE_ENTRIES; return; }

    fill_fib(fib, best);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_examine_fh(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(pkt->dp_Arg2);

    fill_fib(fib, of->node);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* ---- file I/O ---------------------------------------------------------- */

/* Handles FINDINPUT/FINDUPDATE on "debuglog" (case-insensitive, root
 * level only, see is_diaglog_path()) by serving backend_send()'s
 * diag_log() ring buffer (backend.c) as a fresh AllocVec'd snapshot --
 * from here on it's treated exactly like any other successfully-
 * downloaded file (same act_read()/act_end() as real content), so no
 * changes needed anywhere else. Returns 1 if it handled the request
 * (pkt->dp_Res1/Res2 already set), 0 if this wasn't a debuglog request
 * and do_find() should proceed normally. */
static int try_open_diaglog(struct DosPacket *pkt, LONG mode, CNode *base, const char *cpath)
{
    struct FileHandle *fh;
    OpenFile *of;
    UBYTE *snapshot;

    if (!is_diaglog_path(base, cpath)) return 0;

    if (mode == ACTION_FINDOUTPUT) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_WRITE_PROTECTED;
        return 1;
    }

    diaglog_ensure_init();

    snapshot = (UBYTE *)AllocVec(g_diag_pos > 0 ? g_diag_pos : 1, MEMF_PUBLIC);
    if (!snapshot) {
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return 1;
    }
    if (g_diag_pos > 0) u_memcpy(snapshot, g_diag_buf, g_diag_pos);

    of = (OpenFile *)AllocVec(sizeof(OpenFile), MEMF_PUBLIC | MEMF_CLEAR);
    if (!of) {
        FreeVec(snapshot);
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = ERROR_NO_FREE_STORE;
        return 1;
    }
    of->node = &g_diaglog_node;
    of->pos  = 0;
    of->data = snapshot;
    of->size = (LONG)g_diag_pos;
    of->alloc = of->size;
    of->loaded = 1;

    fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    fh->fh_Arg1 = (LONG)of;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
    return 1;
}

static void do_find(struct DosPacket *pkt, LONG mode)
{
    struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    CNode *base = vol_node_from_lock(pkt->dp_Arg2);
    char cpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;
    CNode *node;
    OpenFile *of;
    int created_now = 0;

    bstr_to_cstr(pkt->dp_Arg3, cpath, sizeof(cpath));
    if (try_open_diaglog(pkt, mode, base, cpath)) return;
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    node = rp.node;

    if (mode == ACTION_FINDOUTPUT) {
        if (node) {
            if (node->type != CN_FILE) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE; return; }
            if (node->open_count != 0 || node->excl_lock || node->lock_count != 0) {
                pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
            }
        } else {
            if (rp.leaf[0] == '\0') { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
            node = vol_new_node(rp.dir, rp.leaf, CN_FILE, &g_boot_date);
            if (!node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_NO_FREE_STORE; return; }
            created_now = 1;
        }
        node->excl_lock = 1;
        /* No download: FINDOUTPUT always starts from an empty buffer --
         * the point of it is replacing the file's contents wholesale
         * (docs/handler-plan.md Stufe 2 "Schreiben"). */
    } else if (!node) {
        if (mode != ACTION_FINDUPDATE) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
        if (rp.leaf[0] == '\0') { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
        node = vol_new_node(rp.dir, rp.leaf, CN_FILE, &g_boot_date);
        if (!node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_NO_FREE_STORE; return; }
        created_now = 1;
    } else {
        if (node->type != CN_FILE) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE; return; }
        if (node->excl_lock) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return; }
    }

    of = (OpenFile *)AllocVec(sizeof(OpenFile), MEMF_PUBLIC | MEMF_CLEAR);
    if (!of) {
        if (mode == ACTION_FINDOUTPUT) node->excl_lock = 0;
        if (created_now) {
            /* The node only existed in the cache for this open -- don't
             * leave a file in the tree that the server never saw. */
            vol_unlink(node);
            vol_free_node(node);
        }
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_NO_FREE_STORE; return;
    }
    of->node = node;
    of->pos = 0;
    of->exclusive = (mode == ACTION_FINDOUTPUT);

    if (mode != ACTION_FINDOUTPUT && !created_now) {
        /* An existing file: nothing is transferred yet, reads fetch what
         * they need (act_read()). The size comes from the listing. */
        of->size = node->size;
        of->loaded = 0;
    } else {
        of->loaded = 1; /* new or truncated: empty, and complete */
        /* Opening a new file, or MODE_NEWFILE on an existing one, already
         * changes the server state even if nothing is written before the
         * Close (Copy of an empty file, emptying a file): act_end() must
         * upload the (empty) contents. */
        of->dirty = 1;
    }

    node->open_count++;
    fh->fh_Arg1 = (LONG)of;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_findinput(struct DosPacket *pkt)  { do_find(pkt, ACTION_FINDINPUT); }
void act_findoutput(struct DosPacket *pkt) { do_find(pkt, ACTION_FINDOUTPUT); }
void act_findupdate(struct DosPacket *pkt) { do_find(pkt, ACTION_FINDUPDATE); }

void act_fh_from_lock(struct DosPacket *pkt)
{
    struct FileHandle *fh = (struct FileHandle *)BADDR(pkt->dp_Arg1);
    struct FileLock *fl;
    CNode *node;
    OpenFile *of;

    if (!pkt->dp_Arg2) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
    fl = (struct FileLock *)BADDR(pkt->dp_Arg2);
    node = (CNode *)fl->fl_Key;

    if (node->type != CN_FILE) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE; return; }

    of = (OpenFile *)AllocVec(sizeof(OpenFile), MEMF_PUBLIC | MEMF_CLEAR);
    if (!of) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_NO_FREE_STORE; return; }
    of->node = node;
    of->pos = 0;
    of->exclusive = (fl->fl_Access == EXCLUSIVE_LOCK);

    /* Nothing transferred on open, as in do_find(). */
    of->size = node->size;
    of->loaded = 0;

    /* absorb the lock -- RKRM DOS 14.1.4 */
    if (fl->fl_Access != EXCLUSIVE_LOCK && node->lock_count > 0) node->lock_count--;
    node->open_count++;
    fl->fl_Task = (struct MsgPort *)0;
    FreeVec(fl);

    fh->fh_Arg1 = (LONG)of;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* ---- lazy file content (see actions.h OpenFile) -------------------- */

/* .info files come whole on the first read (WREQ_GET), whatever their
 * size: Workbench needs every byte of an icon. Other files only if they
 * are tiny -- one request either way. */
#define READ_WHOLE_MAX  4096
/* Range sizes for everything else: the first read (Workbench guessing a
 * type for a file without an icon reads a few hundred bytes) costs at
 * most READ_CHUNK_MIN; a program reading on sequentially gets doubling
 * chunks up to READ_CHUNK_MAX, so a Copy still moves in large requests. */
#define READ_CHUNK_MIN  4096
#define READ_CHUNK_MAX  (1024L * 1024L)

static int is_info_name(const char *name)
{
    ULONG n = u_strlen(name);
    return n >= 5 && u_stricmp_latin1(name + n - 5, ".info") == 0;
}

static void drop_window(OpenFile *of)
{
    if (of->win) FreeVec(of->win);
    of->win = (UBYTE *)0;
    of->win_off = 0;
    of->win_len = 0;
}

/* Makes data hold the whole file (WREQ_GET). 0 = ok, else a DOS error;
 * in offline mode (handler.c serve_while_busy()) it changes nothing and
 * the caller's result is discarded anyway. */
static LONG load_whole(OpenFile *of)
{
    char fullpath[AC_PATH_BUF];
    WorkerMsg *reply;
    LONG err;

    if (of->loaded) return 0;
    if (!vol_need_network()) return ERROR_OBJECT_IN_USE;
    vol_build_path(of->node, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_GET, fullpath, (const char *)0, (UBYTE *)0, 0,
                       (const struct DateStamp *)0, 0, (const char *)0, &reply);
    if (err) return err;
    drop_window(of);
    if (of->data) FreeVec(of->data);
    of->data   = reply->buf;
    of->size   = (LONG)reply->buflen;
    of->alloc  = (LONG)reply->buflen;
    of->loaded = 1;
    return 0;
}

/* Fetches the range starting at pos (at least want bytes, see
 * READ_CHUNK_*) into the window. 0 = ok (the window may be shorter than
 * asked at the end of the file), else a DOS error. */
static LONG fetch_window(OpenFile *of, LONG pos, LONG want)
{
    char fullpath[AC_PATH_BUF];
    WorkerMsg *reply;
    LONG err, len;

    if (!vol_need_network()) return ERROR_OBJECT_IN_USE;
    if (of->win && pos == of->win_off + of->win_len) {
        /* reading on sequentially: bigger steps */
        of->next_chunk = of->next_chunk < READ_CHUNK_MAX / 2 ? of->next_chunk * 2 : READ_CHUNK_MAX;
    } else {
        of->next_chunk = READ_CHUNK_MIN;
    }
    len = want > of->next_chunk ? want : of->next_chunk;
    if (len > of->size - pos) len = of->size - pos;

    vol_build_path(of->node, fullpath, sizeof(fullpath));
    err = backend_read(fullpath, pos, (ULONG)len, &reply);
    if (err) return err;
    drop_window(of);
    of->win     = reply->buf;
    of->win_off = pos;
    of->win_len = (LONG)reply->buflen;
    return 0;
}

void act_end(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    CNode *node = of->node;

    if (of->dirty) {
        char fullpath[AC_PATH_BUF];
        LONG err;

        vol_build_path(node, fullpath, sizeof(fullpath));
        err = backend_send(WREQ_PUT, fullpath, (const char *)0, of->data, (ULONG)of->size,
                           (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
        if (err) {
            /* dos.library's Close() frees the FileHandle even when we
             * report an error (dos.doc/Close), and hardly any program
             * retries -- keeping this OpenFile "for a retry" only leaked
             * the whole buffer and left the file exclusively locked
             * until the next mount. So clean up exactly as on success and
             * report the error; the worker has already saved the data
             * to T:AmiOwnCloud-unsaved/ (worker.c save_unsaved()). The tree
             * can't be trusted against the server anymore either way,
             * so force the next access to re-verify. */
            vol_invalidate(node->parent);
            if (node->open_count > 0) node->open_count--;
            if (of->exclusive) node->excl_lock = 0;
            if (of->data) FreeVec(of->data);
            drop_window(of);
            FreeVec(of);
            pkt->dp_Res1 = DOSFALSE;
            pkt->dp_Res2 = err;
            return;
        }
        of->dirty = 0;
        node->size = of->size; /* keep the cache in sync without a full re-list */
        vol_invalidate(node->parent);
    }

    if (node->open_count > 0) node->open_count--;
    if (of->exclusive) node->excl_lock = 0;
    if (of->data) FreeVec(of->data);
    drop_window(of);
    FreeVec(of);

    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_read(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    UBYTE *buf = (UBYTE *)pkt->dp_Arg2;
    LONG want = pkt->dp_Arg3;
    LONG avail, done = 0, pos = of->pos;
    LONG err;

    if (want < 0) { pkt->dp_Res1 = -1; pkt->dp_Res2 = ERROR_SEEK_ERROR; return; }

    avail = of->size - of->pos;
    if (avail < 0) avail = 0;
    if (want > avail) want = avail;

    if (want > 0 && !of->loaded && (of->size <= READ_WHOLE_MAX || is_info_name(of->node->name))) {
        err = load_whole(of);
        if (err) { pkt->dp_Res1 = -1; pkt->dp_Res2 = err; return; }
        if (want > of->size - pos) want = of->size - pos > 0 ? of->size - pos : 0;
    }

    if (of->loaded) {
        if (want > 0) u_memcpy(buf, of->data + pos, (ULONG)want);
        done = want;
    } else {
        /* Bigger file, not loaded: serve from the fetched range, fetch
         * the next one where it runs out. pos is only committed at the
         * end, so a read given up in offline mode leaves nothing changed. */
        while (done < want) {
            LONG off = pos + done;
            if (of->win && off >= of->win_off && off < of->win_off + of->win_len) {
                LONG n = of->win_off + of->win_len - off;
                if (n > want - done) n = want - done;
                u_memcpy(buf + done, of->win + (off - of->win_off), (ULONG)n);
                done += n;
                continue;
            }
            err = fetch_window(of, off, want - done);
            if (err || of->win_len == 0) {
                if (done > 0) break;      /* hand out what we have */
                if (!err) break;          /* server has less than listed: EOF */
                pkt->dp_Res1 = -1; pkt->dp_Res2 = err; return;
            }
        }
    }
    of->pos = pos + done;

    pkt->dp_Res1 = done;
    pkt->dp_Res2 = 0;
}

void act_write(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    const UBYTE *buf = (const UBYTE *)pkt->dp_Arg2;
    LONG want = pkt->dp_Arg3;
    LONG need_end;

    if (want < 0) { pkt->dp_Res1 = -1; pkt->dp_Res2 = ERROR_SEEK_ERROR; return; }
    if (!of->loaded) {
        LONG err = load_whole(of); /* changing a file needs all of it */
        if (err) { pkt->dp_Res1 = -1; pkt->dp_Res2 = err; return; }
    }

    need_end = of->pos + want;
    if (need_end > of->alloc && !vol_grow_buf(&of->data, &of->alloc, of->size, need_end)) {
        pkt->dp_Res1 = -1; pkt->dp_Res2 = ERROR_DISK_FULL; return;
    }
    if (want > 0) u_memcpy(of->data + of->pos, buf, (ULONG)want);
    of->pos += want;
    if (of->pos > of->size) of->size = of->pos;
    of->dirty = 1;

    pkt->dp_Res1 = want;
    pkt->dp_Res2 = 0;
}

void act_seek(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    LONG offset = pkt->dp_Arg2;
    LONG mode = pkt->dp_Arg3;
    LONG base, newpos, oldpos = of->pos;

    /* RKRM DOS 14.1.8: be permissive -- negative mode == BEGINNING,
     * positive == END, only exactly 0 is CURRENT. */
    if (mode < 0) base = 0;
    else if (mode == 0) base = of->pos;
    else base = of->size;

    newpos = base + offset;
    if (newpos < 0 || newpos > of->size) {
        pkt->dp_Res1 = -1; pkt->dp_Res2 = ERROR_SEEK_ERROR; return;
    }
    of->pos = newpos;
    pkt->dp_Res1 = oldpos;
    pkt->dp_Res2 = 0;
}

void act_set_file_size(struct DosPacket *pkt)
{
    OpenFile *of = (OpenFile *)pkt->dp_Arg1;
    LONG offset = pkt->dp_Arg2;
    LONG mode = pkt->dp_Arg3;
    LONG base, newsize;

    if (mode < 0) base = 0;
    else if (mode == 0) base = of->pos;
    else base = of->size;

    newsize = base + offset;
    if (newsize < 0) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_SEEK_ERROR; return; }
    if (!of->loaded) {
        LONG err = load_whole(of);
        if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    }
    if (newsize > of->alloc && !vol_grow_buf(&of->data, &of->alloc, of->size, newsize)) {
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_DISK_FULL; return;
    }
    /* Extending: the new bytes are zeros, not whatever the buffer held
     * (fresh AllocVec memory, or data cut off by an earlier truncate). */
    if (newsize > of->size) u_memset(of->data + of->size, 0, (ULONG)(newsize - of->size));
    of->size = newsize;
    if (of->pos > of->size) of->pos = of->size;
    of->dirty = 1;

    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* ---- metadata / tree mutation ----------------------------------------- */

void act_delete_object(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg1);
    char cpath[AC_PATH_BUF], fullpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;
    CNode *parent;

    bstr_to_cstr(pkt->dp_Arg2, cpath, sizeof(cpath));
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    if (!rp.node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
    if (rp.node == g_root) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return; }
    if (rp.node->lock_count != 0 || rp.node->excl_lock || rp.node->open_count != 0) {
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
    }
    if (rp.node->type == CN_DIR && rp.node->children) {
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_DIRECTORY_NOT_EMPTY; return;
    }

    vol_build_path(rp.node, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_DEL, fullpath, (const char *)0, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    parent = rp.node->parent;
    vol_unlink(rp.node);
    /* Someone may have locked it while the request was out (Lock is
     * answered during a running request, handler.c serve_while_busy()):
     * then it stays allocated -- detached, so nothing finds it anymore --
     * rather than leaving that lock pointing at freed memory. */
    if (rp.node->lock_count == 0 && !rp.node->excl_lock && rp.node->open_count == 0)
        vol_free_node(rp.node);
    vol_invalidate(parent);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

static void append_child(CNode *dir, CNode *node)
{
    CNode **pp = &dir->children;
    while (*pp) pp = &(*pp)->next;
    *pp = node;
}

void act_rename_object(struct DosPacket *pkt)
{
    CNode *srcbase = vol_node_from_lock(pkt->dp_Arg1);
    CNode *dstbase = vol_node_from_lock(pkt->dp_Arg3);
    char spath[AC_PATH_BUF], dpath[AC_PATH_BUF];
    char srcfull[AC_PATH_BUF], dstfull[AC_PATH_BUF];
    ResolvedPath srp, drp;
    LONG err;
    CNode *src, *conflict, *old_parent;

    bstr_to_cstr(pkt->dp_Arg2, spath, sizeof(spath));
    bstr_to_cstr(pkt->dp_Arg4, dpath, sizeof(dpath));

    err = resolve(srcbase, spath, &srp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    src = srp.node;
    if (!src) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
    if (src == g_root) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return; }

    err = resolve(dstbase, dpath, &drp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    old_parent = src->parent;

    if (drp.leaf[0] == '\0') {
        /* whole target path resolved to an existing directory -> move src
         * into it under its own name (RKRM DOS 14.5.1) */
        if (!drp.node || drp.node->type != CN_DIR) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }
        conflict = vol_find_child(drp.node, src->name);
        if (conflict && conflict != src) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_EXISTS; return; }
        if (src->type == CN_DIR && vol_is_ancestor(src, drp.node)) {
            pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
        }

        vol_build_path(src, srcfull, sizeof(srcfull));
        vol_child_path(drp.node, src->name, dstfull, sizeof(dstfull));
        err = backend_send(WREQ_RENAME, srcfull, dstfull, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
        if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

        vol_unlink(src);
        src->parent = drp.node;
        append_child(drp.node, src);
        vol_invalidate(old_parent);
        vol_invalidate(drp.node);
        pkt->dp_Res1 = DOSTRUE; pkt->dp_Res2 = 0;
        return;
    }

    if (drp.node) {
        if (drp.node == src) {
            /* case-only rename in place */
            vol_build_path(src, srcfull, sizeof(srcfull));
            vol_child_path(drp.dir, drp.leaf, dstfull, sizeof(dstfull));
            err = backend_send(WREQ_RENAME, srcfull, dstfull, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
            if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

            u_strncpy(src->name, drp.leaf, sizeof(src->name));
            vol_invalidate(old_parent);
            pkt->dp_Res1 = DOSTRUE; pkt->dp_Res2 = 0;
            return;
        }
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_EXISTS; return;
    }

    if (src->type == CN_DIR && vol_is_ancestor(src, drp.dir)) {
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_IN_USE; return;
    }

    vol_build_path(src, srcfull, sizeof(srcfull));
    vol_child_path(drp.dir, drp.leaf, dstfull, sizeof(dstfull));
    err = backend_send(WREQ_RENAME, srcfull, dstfull, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    vol_unlink(src);
    u_strncpy(src->name, drp.leaf, sizeof(src->name));
    src->parent = drp.dir;
    append_child(drp.dir, src);
    vol_invalidate(old_parent);
    vol_invalidate(drp.dir);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* SET_PROTECT/SET_COMMENT/SET_DATE take their lock in dp_Arg2, not
 * dp_Arg1 -- see the citation block at the top of this file. */

void act_set_protect(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg2);
    char cpath[AC_PATH_BUF], fullpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;

    bstr_to_cstr(pkt->dp_Arg3, cpath, sizeof(cpath));
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    if (!rp.node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }

    vol_build_path(rp.node, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_SETPROTECT, fullpath, (const char *)0, (UBYTE *)0, 0,
                       (const struct DateStamp *)0, pkt->dp_Arg4, (const char *)0, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    rp.node->protection = pkt->dp_Arg4;
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_set_comment(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg2);
    char cpath[AC_PATH_BUF], fullpath[AC_PATH_BUF];
    char comment[CNODE_MAXCOMMENT + 1];
    ResolvedPath rp;
    LONG err;

    bstr_to_cstr(pkt->dp_Arg3, cpath, sizeof(cpath));
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    if (!rp.node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }

    if (bstr_len(pkt->dp_Arg4) > CNODE_MAXCOMMENT) {
        pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_COMMENT_TOO_BIG; return;
    }
    bstr_to_cstr(pkt->dp_Arg4, comment, sizeof(comment));

    vol_build_path(rp.node, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_SETCOMMENT, fullpath, (const char *)0, (UBYTE *)0, 0,
                       (const struct DateStamp *)0, 0, comment, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    u_strncpy(rp.node->comment, comment, sizeof(rp.node->comment));
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_set_date(struct DosPacket *pkt)
{
    CNode *base = vol_node_from_lock(pkt->dp_Arg2);
    char cpath[AC_PATH_BUF], fullpath[AC_PATH_BUF];
    ResolvedPath rp;
    LONG err;
    struct DateStamp *ds;

    bstr_to_cstr(pkt->dp_Arg3, cpath, sizeof(cpath));
    err = resolve(base, cpath, &rp);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }
    if (!rp.node) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = ERROR_OBJECT_NOT_FOUND; return; }

    /* dp_Arg4 is a CPTR (plain pointer), not a BPTR -- BADDR() read
     * four times the address and set every date to 01-Jan-78 */
    ds = (struct DateStamp *)pkt->dp_Arg4;

    vol_build_path(rp.node, fullpath, sizeof(fullpath));
    err = backend_send(WREQ_SETDATE, fullpath, (const char *)0, (UBYTE *)0, 0,
                       ds, 0, (const char *)0, (WorkerMsg **)0);
    if (err) { pkt->dp_Res1 = DOSFALSE; pkt->dp_Res2 = err; return; }

    u_memcpy(&rp.node->date, ds, sizeof(struct DateStamp));
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

/* ---- volume-level ------------------------------------------------------- */

/* Fixed placeholder capacity -- refined in Stage 3 to reflect real tree
 * size (docs/handler-plan.md "DISK_INFO mit plausiblen Zahlen"). */
static void fill_info(struct InfoData *id)
{
    id->id_NumSoftErrors = 0;
    id->id_UnitNumber = 0;
    id->id_DiskState = ID_VALIDATED;
    id->id_NumBlocks = 0x003FFFFF;
    id->id_NumBlocksUsed = id->id_NumBlocks / 2;
    id->id_BytesPerBlock = 512;
    id->id_DiskType = ID_DOS_DISK;
    id->id_VolumeNode = g_volume_bptr;
    id->id_InUse = 1;
}

void act_info(struct DosPacket *pkt)
{
    struct InfoData *id = (struct InfoData *)BADDR(pkt->dp_Arg2);
    fill_info(id);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_disk_info(struct DosPacket *pkt)
{
    struct InfoData *id = (struct InfoData *)BADDR(pkt->dp_Arg1);
    fill_info(id);
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_current_volume(struct DosPacket *pkt)
{
    pkt->dp_Res1 = g_volume_bptr;
    pkt->dp_Res2 = 0;
}

void act_is_filesystem(struct DosPacket *pkt)
{
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}

void act_flush(struct DosPacket *pkt)
{
    pkt->dp_Res1 = DOSTRUE;
    pkt->dp_Res2 = 0;
}
