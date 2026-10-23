#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# AmiOwnCloudSync-Ordnerpaar-Regressionstest -- kompiliert und laeuft nativ
# auf dem Host (kein Amiga/vbcc noetig), gegen die ECHTEN Projekt-Quellen
# apps/amiowncloud-sync-cx/src/pairs.c und
# core/libcloudcore/src/config.c.
#
# Prueft den Ordnerpaar-Speicher/Lade-Round-Trip (pairs_save/pairs_load),
# u.a. den 2026-07-15 gefundenen Bug: zu kleine line[]/val[]-Puffer beim
# Parsen liessen lange Pfade/Werte fgets() "mitten in der Zeile" abbrechen,
# wodurch der Rest der Zeile -- und damit das ganze Ordnerpaar -- beim
# naechsten fgets()-Aufruf lautlos verworfen wurde.
#
# Aufruf: scripts/test_pairs.sh

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYNC_CX="$ROOT/apps/amiowncloud-sync-cx"
CORE="$ROOT/core/libcloudcore"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cc -Wall -Wextra \
    -I "$CORE/include/cloudcore" \
    -I "$SYNC_CX/src" \
    "$SYNC_CX/tests/test_pairs.c" \
    "$SYNC_CX/src/pairs.c" \
    "$CORE/src/config.c" \
    -o "$OUT/test_pairs"

"$OUT/test_pairs"
