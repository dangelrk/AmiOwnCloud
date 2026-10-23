/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef OWNCLOUDHANDLER_VOL_H
#define OWNCLOUDHANDLER_VOL_H

/*
 * vol.h -- the RAM tree behind CLOUD:. Stage 1 was the only copy of the
 * data (a plain RAM filesystem); from Stage 2 on this is a *cache* of
 * server state (docs/handler-plan.md Stufe 2):
 *   - CNode no longer holds file content -- that lives per-open-handle on
 *     OpenFile (actions.h) instead, fetched fresh via WREQ_GET on
 *     FINDINPUT/FINDUPDATE/FH_FROM_LOCK and pushed via WREQ_PUT from
 *     ACTION_END. A CNode's size/protection/comment/date are metadata
 *     mirrored from the server (list/info replies), not authoritative.
 *   - Directory listings are cached with a ~10s TTL: vol_find_child()
 *     transparently refreshes a stale directory before searching it, so
 *     every existing lookup in actions.c became cache-aware for free.
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>

/* fib_FileName[108] is a BSTR at the packet level -- length byte + up to
 * 106 chars fits exactly (see util.h fill_bstr_field). fib_Comment[80]
 * likewise caps at 79 (RKRM DOS 14.5.4 and docs/API.md). */
#define CNODE_MAXNAME    106
#define CNODE_MAXCOMMENT 79

/* Default directory listing cache lifetime, in the same units as
 * WorkerMsg's server_now (seconds, worker's clock -- see vol_note_now()).
 * The config's cache_ttl= overrides it (arrives with every worker reply,
 * see backend_cache_ttl()); over a modem a longer one saves a lot of
 * slow re-listings. */
#define CACHE_TTL 30

typedef enum { CN_FILE, CN_DIR } CNodeType;

typedef struct CNode {
    struct CNode *parent;
    struct CNode *next;      /* next sibling in parent->children */
    struct CNode *children;  /* first child (CN_DIR only) */
    CNodeType     type;
    char          name[CNODE_MAXNAME + 1];
    LONG          size;      /* cached from the server (list/info), or set
                               * from the just-uploaded buffer's length
                               * after a successful ACTION_END/WREQ_PUT */
    LONG          protection; /* raw Amiga bits, RWED active-low, see docs/API.md */
    char          comment[CNODE_MAXCOMMENT + 1];
    struct DateStamp date;
    LONG          lock_count; /* outstanding shared locks */
    int           excl_lock;  /* 1 = an exclusive lock is held */
    LONG          open_count; /* outstanding open FileHandles (any mode) */
    ULONG         seq;        /* creation order; used as fib_DiskKey (see
                                * actions.c ACTION_EXAMINE_NEXT) -- global,
                                * monotonic, assigned once, never reused. */

    /* Stage 2: directory listing cache (CN_DIR only, see vol_refresh()). */
    int           listed;     /* 0 = never WREQ_LIST'd or invalidated -- stale */
    int           ever_listed; /* 1 once a listing arrived; children are real
                                * data from then on, even when stale */
    LONG          cache_time; /* g_now (vol_note_now()) at last refresh */
    int           seen;       /* scratch flag, valid only during a single
                                * vol_refresh() merge pass */

    /* Crash-hunt diagnostic (temporary): 1 for the single synthetic
     * "debuglog" root-level file created in vol_init() (see backend.c's
     * diag_log() ring buffer) -- a way to retrieve backend_send()'s own
     * trace of Wait()/GetMsg()/AbortIO()/WaitIO() after a hang, since the
     * handler can never safely write a real log file to disk from inside
     * the packet loop (the exact deadlock risk the two-process split
     * exists to avoid). vol_refresh()'s merge loop must never cull this
     * node (the server never lists it), and do_find() must serve its
     * content from the ring buffer instead of a real WREQ_GET. */
    int           is_diaglog;
} CNode;

extern CNode *g_root;

/* Allocates and initialises g_root. Returns 1 on success, 0 on AllocVec
 * failure (caller must abort ACTION_STARTUP in that case). */
int    vol_init(const struct DateStamp *boot_date);

/* Allocates a new node and appends it to parent->children (parent may be
 * NULL only for the one-time g_root allocation done by vol_init itself).
 * Returns NULL on AllocVec failure. */
CNode *vol_new_node(CNode *parent, const char *name, CNodeType type, const struct DateStamp *date);

/* Detaches node from its parent's child list (does not free it). */
void   vol_unlink(CNode *node);

/* Frees node itself (no per-node buffer to free anymore, see file header).
 * node must already be unlinked, unlocked (lock_count==0, !excl_lock) and
 * closed (open_count==0). */
void   vol_free_node(CNode *node);

/* Latin-1 case-insensitive child lookup by name. Transparently refreshes
 * dir first if its listing cache is stale (vol_ensure_fresh()) -- every
 * caller in actions.c that walks a path becomes cache-aware for free.
 * NULL with vol_lookup_error() != 0 means the directory could not be
 * listed at all (server unreachable), not that the name doesn't exist. */
CNode *vol_find_child(CNode *dir, const char *name);
LONG   vol_lookup_error(void);

/* 1 if `node` is `maybe_ancestor` itself or a descendant of it -- used to
 * reject "move a directory into itself or one of its own subdirectories"
 * (RKRM DOS 14.5.1, footnote on ACTION_RENAME_OBJECT). */
int    vol_is_ancestor(CNode *maybe_ancestor, CNode *node);

/* Ensures *alloc >= need_size for a generic AllocVec'd growable buffer
 * (doubling from 16KB), copying the first cur_size bytes of the old
 * buffer over. Used by actions.c for OpenFile's per-handle read/write
 * buffer (Stage 1 used this directly on CNode; see file header for why
 * that moved). Returns 1 on success, 0 on AllocVec failure (caller should
 * report ERROR_NO_FREE_STORE / ERROR_DISK_FULL). */
int    vol_grow_buf(UBYTE **data, LONG *alloc, LONG cur_size, LONG need_size);

ULONG  vol_next_seq(void);

/* ---- path <-> tree ------------------------------------------------- */
/* Reconstructs node's absolute server path ("/", "/a", "/a/b", ...) by
 * walking parent pointers -- the wire protocol (proto.h) addresses
 * objects by path, not by our local identity. */
void   vol_build_path(CNode *node, char *out, ULONG outsz);

/* vol_build_path(dir) + "/" + leaf, handling dir==root ("/") without a
 * doubled slash. */
void   vol_child_path(CNode *dir, const char *leaf, char *out, ULONG outsz);

/* ---- directory listing cache (Stage 2) -------------------------------- */
/* Records the worker's clock (WorkerMsg.server_now) as our notion of
 * "now" -- the packet loop has no clock access of its own (exec.library
 * only), so this is the only source of "now" there is; see file header. */
void   vol_note_now(LONG server_now);

/* Refreshes dir (must be CN_DIR) via a synchronous WREQ_LIST + merge:
 * updates/creates/removes children to match the server, except any child
 * that's locked, exclusively locked, or open, which always survives
 * untouched (docs/handler-plan.md Stufe 2: "gelockte/geoeffnete Knoten
 * bleiben"). Leaves the existing cache as-is on an error and returns the
 * DOS error code (0 = ok). */
LONG   vol_refresh(CNode *dir);

/* Refreshes dir if its listing cache is stale (never listed, or older
 * than the cache lifetime) -- called automatically by vol_find_child();
 * exposed directly for ACTION_EXAMINE_*, which walk dir->children
 * without going through vol_find_child(). Returns an error only if dir
 * has never been listed and the listing failed -- an older listing is
 * still served (stale data beats no data, the next access retries),
 * but an empty never-listed directory must not pass for the real one. */
LONG   vol_ensure_fresh(CNode *dir);

/* 1 if using dir (or a file) right now needs no network round trip. */
int    vol_is_fresh(CNode *dir);

/* Offline mode for answering packets while a worker request is running
 * (handler.c serve_while_busy()): with it on, a directory that would need
 * a refresh is not refreshed; vol_ensure_fresh() fails instead and
 * vol_would_block() turns 1, telling the caller to queue the packet and
 * run it again later, ignoring whatever result it produced. */
void   vol_set_offline(int on);
int    vol_would_block(void);

/* Forces dir's *next* access to refresh regardless of TTL (docs/handler-
 * plan.md Stufe 2: mutations "invalidieren Parent" after applying their
 * own change to the tree directly). Safe to call on NULL. */
void   vol_invalidate(CNode *dir);

/* ---- locks ----------------------------------------------------------- */
/* BPTR lock (0 = root) -> CNode*, via struct FileLock.fl_Key. */
CNode *vol_node_from_lock(BPTR lock);

/* Creates a struct FileLock (AllocVec'd) on node, enforcing the invariant
 * from docs/handler-plan.md: exclusive only if lock_count==0 && !excl_lock
 * && open_count==0; root can never be locked exclusively. Any access value
 * other than EXCLUSIVE_LOCK is treated as SHARED_LOCK (RKRM DOS 14.2.1).
 * Returns a BPTR (MKBADDR) on success, 0 with *err set on failure. */
BPTR   vol_lock_create(CNode *node, LONG access, struct MsgPort *task, BPTR volume_bptr, LONG *err);

/* Releases a lock created by vol_lock_create (ignores lock==0). */
void   vol_lock_release(BPTR lock);

#endif /* OWNCLOUDHANDLER_VOL_H */
