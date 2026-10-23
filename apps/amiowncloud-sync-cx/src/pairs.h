/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef PAIRS_H
#define PAIRS_H

/*
 * pairs.h -- AmiOwnCloudSync's own preferences: the OwnCloud profile
 * (server/username/password/verify_ssl, loaded via libcloudcore's
 * config_load/config_merge exactly like every other app here) PLUS this
 * commodity's own extra keys (interval=, repeated pair= lines) in the
 * SAME file. Deliberately its own module, not part of libcloudcore --
 * "a list of local<->remote folder pairs to autosync" is this
 * commodity's own business, nothing else in the repo needs it.
 *
 * File format (see docs, ENVARC:AmiOwnCloudSync.cfg) -- user=/pass=, the
 * same key names every other config file in this repo uses
 * (amicloud.cfg.example, AmiCloud/AMIVCS docs), not username=/password=:
 *   server=https://cloud.example.com/remote.php/dav/files/USER
 *   user=USER
 *   pass=APP-PASSWORT
 *   interval=15
 *   pair=Work:Dokumente|/Dokumente|on
 *   pair=RAM:Projekte|/Projekte|on
 *
 * A pair's local field is a plain Amiga path (volume:dir/subdir or a
 * relative path) -- it doubles as its own display label, no separate
 * name needed. remote is a WebDAV-relative path under the profile's
 * server URL, "/" meaning "the whole cloud".
 */

#include "amicloud.h"

typedef struct {
    char local[AC_MAX_PATH];
    char remote[AC_MAX_PATH];
    int  enabled;
} SyncPair;

typedef struct {
    SyncPair *items;
    int n, cap;
    int interval_minutes; /* autosync period; <=0 means "no autosync" */
} PairList;

void pairs_init(PairList *pl);

/* Reads config_load()'s usual fields into cfg AND this file's own
 * interval=/pair= lines into pl. A missing file is not an error -- cfg
 * and pl come back at their zeroed/default state (first run). Returns
 * 1 on success (including "file missing"), 0 only on allocation failure. */
int pairs_load(const char *filepath, AcConfig *cfg, PairList *pl);

/* Serializes cfg + pl back to filepath in the format above, atomically
 * (writes a .tmp file, then renames it over the target) -- an
 * interrupted save must never leave a half-written prefs file behind. */
int pairs_save(const char *filepath, const AcConfig *cfg, const PairList *pl);

void pairs_add(PairList *pl, const char *local, const char *remote, int enabled);
void pairs_remove(PairList *pl, int index);
void pairs_free(PairList *pl);

#endif /* PAIRS_H */
