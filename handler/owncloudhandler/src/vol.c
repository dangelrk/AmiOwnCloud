/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "vol.h"
#include "util.h"
#include "backend.h"

#include <exec/memory.h>
#include <dos/dosextens.h>
#include <proto/exec.h>

CNode *g_root;

static ULONG g_seq = 0;
static LONG  g_now = 0; /* see vol_note_now() */
static LONG  g_lookup_err = 0; /* see vol_lookup_error() */

ULONG vol_next_seq(void)
{
    return ++g_seq;
}

int vol_init(const struct DateStamp *boot_date)
{
    g_root = (CNode *)AllocVec(sizeof(CNode), MEMF_PUBLIC | MEMF_CLEAR);
    if (!g_root) return 0;

    g_root->type = CN_DIR;
    g_root->name[0] = '\0';
    if (boot_date) u_memcpy(&g_root->date, boot_date, sizeof(struct DateStamp));
    g_root->seq = vol_next_seq();
    return 1;
}

CNode *vol_new_node(CNode *parent, const char *name, CNodeType type, const struct DateStamp *date)
{
    CNode *n;
    CNode **pp;

    n = (CNode *)AllocVec(sizeof(CNode), MEMF_PUBLIC | MEMF_CLEAR);
    if (!n) return (CNode *)0;

    u_strncpy(n->name, name, sizeof(n->name));
    n->type = type;
    if (date) u_memcpy(&n->date, date, sizeof(struct DateStamp));
    n->seq = vol_next_seq();
    n->parent = parent;

    if (parent) {
        pp = &parent->children;
        while (*pp) pp = &(*pp)->next;
        *pp = n;
    }
    return n;
}

void vol_unlink(CNode *node)
{
    CNode **pp;

    if (!node->parent) return;
    pp = &node->parent->children;
    while (*pp && *pp != node) pp = &(*pp)->next;
    if (*pp == node) *pp = node->next;
    node->next = (CNode *)0;
    node->parent = (CNode *)0;
}

void vol_free_node(CNode *node)
{
    FreeVec(node);
}

/* Plain by-name search over dir's existing children, no refresh side
 * effect. Needed as its own function (not just vol_find_child()'s body)
 * because vol_refresh()'s own merge loop below must search without
 * triggering vol_ensure_fresh() again -- calling vol_find_child() from
 * there used to re-enter vol_refresh() on the same dir (dir->listed isn't
 * set to 1 until the merge loop finishes, so vol_ensure_fresh() saw it as
 * still stale on every single entry of the very first listing of any
 * directory) -- unbounded recursion, one more backend_send() round trip
 * and stack frame per entry, straight into a stack overflow. Reported as
 * a full system freeze on the very first `Dir CLOUD:` after mount. */
static CNode *vol_find_child_raw(CNode *dir, const char *name)
{
    CNode *c;

    for (c = dir->children; c; c = c->next)
        if (u_stricmp_latin1(c->name, name) == 0) return c;
    return (CNode *)0;
}

CNode *vol_find_child(CNode *dir, const char *name)
{
    g_lookup_err = 0;
    if (!dir) return (CNode *)0;
    g_lookup_err = vol_ensure_fresh(dir);
    if (g_lookup_err) return (CNode *)0;
    return vol_find_child_raw(dir, name);
}

LONG vol_lookup_error(void)
{
    return g_lookup_err;
}

int vol_is_ancestor(CNode *maybe_ancestor, CNode *node)
{
    CNode *p;

    for (p = node; p; p = p->parent)
        if (p == maybe_ancestor) return 1;
    return 0;
}

int vol_grow_buf(UBYTE **data, LONG *alloc, LONG cur_size, LONG need_size)
{
    LONG newalloc;
    UBYTE *newdata;

    if (need_size <= *alloc) return 1;

    newalloc = *alloc ? *alloc : 16384L;
    while (newalloc < need_size) newalloc <<= 1;

    newdata = (UBYTE *)AllocVec((ULONG)newalloc, MEMF_PUBLIC);
    if (!newdata) return 0;

    if (*data) {
        if (cur_size > 0) u_memcpy(newdata, *data, (ULONG)cur_size);
        FreeVec(*data);
    }
    *data  = newdata;
    *alloc = newalloc;
    return 1;
}

/* ---- path <-> tree ------------------------------------------------- */

void vol_build_path(CNode *node, char *out, ULONG outsz)
{
    CNode *stack[64];
    int n = 0;
    CNode *p = node;
    ULONG pos = 0;

    while (p && p->parent && n < 64) { stack[n++] = p; p = p->parent; }

    if (!outsz) return;
    if (n == 0) { u_strncpy(out, "/", outsz); return; }

    out[0] = '\0';
    while (n > 0) {
        ULONG len;
        n--;
        if (pos + 1 < outsz) { out[pos++] = '/'; out[pos] = '\0'; }
        len = u_strlen(stack[n]->name);
        if (pos + len < outsz) {
            u_memcpy(out + pos, stack[n]->name, len);
            pos += len;
            out[pos] = '\0';
        }
    }
}

void vol_child_path(CNode *dir, const char *leaf, char *out, ULONG outsz)
{
    ULONG len;

    vol_build_path(dir, out, outsz);
    len = u_strlen(out);
    if (len > 0 && out[len - 1] != '/' && len + 1 < outsz) {
        out[len] = '/';
        out[len + 1] = '\0';
        len++;
    }
    u_strncpy(out + len, leaf, outsz > len ? outsz - len : 0);
}

/* ---- directory listing cache (Stage 2) -------------------------------- */

void vol_note_now(LONG server_now)
{
    if (server_now > g_now) g_now = server_now;
}

void vol_invalidate(CNode *dir)
{
    if (dir) dir->listed = 0;
}

LONG vol_refresh(CNode *dir)
{
    char path[512];
    LONG err;
    WorkerMsg *reply;
    int i;
    CNode *c, *next;

    if (dir->type != CN_DIR) return 0;

    vol_build_path(dir, path, sizeof(path));
    err = backend_send(WREQ_LIST, path, (const char *)0, (UBYTE *)0, 0, (const struct DateStamp *)0, 0, (const char *)0, &reply);
    if (err) return err; /* leave the existing cache as-is; next access retries */

    vol_note_now(reply->server_now);

    for (c = dir->children; c; c = c->next) c->seen = 0;

    for (i = 0; i < reply->n_entries; i++) {
        WEntry *e = &reply->entries[i];
        CNodeType t = e->is_dir ? CN_DIR : CN_FILE;
        CNode *ch = vol_find_child_raw(dir, e->name);

        if (ch && (ch->lock_count != 0 || ch->excl_lock || ch->open_count != 0)) {
            /* In use locally right now -- keep it exactly as-is, just
             * don't let the cull pass below remove it. */
            ch->seen = 1;
            continue;
        }

        if (ch && ch->type != t) {
            vol_unlink(ch);
            vol_free_node(ch);
            ch = (CNode *)0;
        }

        if (!ch) {
            ch = vol_new_node(dir, e->name, t, (const struct DateStamp *)0);
            if (!ch) continue; /* out of memory -- skip, retried next refresh */
        }

        ch->seen = 1;
        ch->size = (t == CN_DIR) ? 0 : e->size;
        ch->protection = e->protection;
        u_strncpy(ch->comment, e->comment, sizeof(ch->comment));
        u_memcpy(&ch->date, &e->mtime, sizeof(struct DateStamp)); /* already
            * converted worker-side, see worker.c epoch_to_datestamp() */
    }

    c = dir->children;
    while (c) {
        next = c->next;
        if (!c->seen && c->lock_count == 0 && !c->excl_lock && c->open_count == 0) {
            vol_unlink(c);
            vol_free_node(c);
        }
        c = next;
    }

    dir->listed = 1;
    dir->ever_listed = 1;
    dir->cache_time = g_now;

    if (reply->entries) FreeVec(reply->entries);
    return 0;
}

int vol_is_fresh(CNode *dir)
{
    LONG now = g_now > backend_now() ? g_now : backend_now();
    LONG ttl = backend_cache_ttl() > 0 ? backend_cache_ttl() : CACHE_TTL;

    if (dir->type != CN_DIR) return 1;
    return dir->listed && (now - dir->cache_time) <= ttl;
}

LONG vol_ensure_fresh(CNode *dir)
{
    int never_listed;
    LONG err;

    if (vol_is_fresh(dir)) return 0;
    never_listed = !dir->ever_listed;
    err = vol_refresh(dir);
    return never_listed ? err : 0;
}

/* ---- locks ------------------------------------------------------------ */

CNode *vol_node_from_lock(BPTR lock)
{
    struct FileLock *fl;

    if (!lock) return g_root;
    fl = (struct FileLock *)BADDR(lock);
    return (CNode *)fl->fl_Key;
}

BPTR vol_lock_create(CNode *node, LONG access, struct MsgPort *task, BPTR volume_bptr, LONG *err)
{
    struct FileLock *fl;

    if (access == EXCLUSIVE_LOCK) {
        if (node == g_root || node->lock_count != 0 || node->excl_lock || node->open_count != 0) {
            *err = ERROR_OBJECT_IN_USE;
            return 0;
        }
    } else {
        /* RKRM DOS 14.2.1: values other than EXCLUSIVE_LOCK are treated as
         * SHARED_LOCK, since some callers pass garbage in dp_Arg3. */
        access = SHARED_LOCK;
        if (node->excl_lock) {
            *err = ERROR_OBJECT_IN_USE;
            return 0;
        }
    }

    fl = (struct FileLock *)AllocVec(sizeof(struct FileLock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fl) { *err = ERROR_NO_FREE_STORE; return 0; }

    fl->fl_Link   = 0;
    fl->fl_Key    = (LONG)node;
    fl->fl_Access = access;
    fl->fl_Task   = task;
    fl->fl_Volume = volume_bptr;

    if (access == EXCLUSIVE_LOCK) node->excl_lock = 1;
    else node->lock_count++;

    *err = 0;
    return MKBADDR(fl);
}

void vol_lock_release(BPTR lock)
{
    struct FileLock *fl;
    CNode *node;

    if (!lock) return;
    fl = (struct FileLock *)BADDR(lock);
    node = (CNode *)fl->fl_Key;

    /* RKRM DOS 14.2.9: zero fl_Task first as a precaution against a
     * double-unlock racing us (dos.library ignores locks with no task). */
    fl->fl_Task = (struct MsgPort *)0;

    if (fl->fl_Access == EXCLUSIVE_LOCK) node->excl_lock = 0;
    else if (node->lock_count > 0) node->lock_count--;

    FreeVec(fl);
}
