# Implementierungsplan: Sync-Metadaten (07_Sync_Metadaten.md)

> Stand: 2026-07-12 · Umsetzungsplan zum Konzept `docs/architecture/07_Sync_Metadaten.md` ·
> HTML-Fassung: `07_Sync_Metadaten_Plan.html` · Nur Planung — Umbau folgt nach Review.

## Context

`docs/architecture/07_Sync_Metadaten.md` beschreibt ein metadatenbasiertes Sync-Verfahren mit
Last-Sync-Baseline, 3-Wege-Konflikterkennung, Löscherkennung, MD5-Cache sowie serverseitigen
Revisionen und einer globalen Server-Revision. Die **heutige Implementierung leistet nichts davon**:

- `core/libcloudsync/src/sync.c` (`cloudsync_run()`) ist ein **zustandsloser 2-Wege-Reconciler**.
  Er baut bei jedem Lauf beide Bäume neu auf, vergleicht nur Local↔Remote (Größe → MD5 → mtime)
  und verwirft alles danach. Es gibt **keine Baseline**, daher:
  - **Löschungen werden nie erkannt** — eine auf einer Seite fehlende Datei gilt immer als „neu auf
    der anderen Seite" und wird wieder angelegt (`sync.c:258-291`).
  - **Konflikte** werden rein per mtime-Heuristik entschieden (`queue_transfer_by_mtime`, `sync.c:192`).
  - MD5 wird **jedes Mal neu berechnet**, kein Cache (`sync.c:311`).
- Server (`server/`): `files`-Tabelle hat **keine** `revision`, **keinen** globalen Zähler, und löscht
  **hart** (`FileStore.php` `deleteImpl()`), ohne Tombstone. `id`/`user_id`/`created_at` existieren,
  werden aber nicht an den Client ausgeliefert.

Ziel dieses Plans: das in 07 beschriebene Modell voll umsetzen — serverseitige Revisionen +
globale (pro-User) Revision + Soft-Delete, clientseitige Last-Sync-DB (`.amicloudsync` als JSON im
Sync-Wurzelverzeichnis), 3-Wege-Diff mit Lösch- und Konflikterkennung, MD5-Cache und die
Delta-Optimierung über die globale Revision. Grundregel aus dem Doc bleibt bindend:
**niemals eine Datei unbemerkt überschreiben**, und die Baseline (Last Sync State) wird **erst nach
vollständig erfolgreichem Sync** geschrieben.

Randbedingungen: **PHP 7.4.33** serverseitig (kein `match`/enums/union-types/`never`).
Client muss auf **vbcc/AmigaOS 3 (m68k, C89)** cross-compilen; `long` ist dort 32-bit
(mtime bis 2038 und Dateigrößen < 2 GB unkritisch für Amiga-Ziele; parson führt Zahlen als `double`,
Ganzzahlen < 2⁵³ round-trippen über `%1.17g` exakt — siehe A1).
Modul-Kapselung (`docs/architecture/05_Refactoring_Client_Handler.md:99-116`) bleibt bindend:
Typen → core, Zeit → core, MD5 → core, HTTP/URL/Auth → http, FS → fs, Sync-Domäne → libcloudsync.

---

## Phase B (Server) — Revision, globale Revision, Soft-Delete

Zuerst der Server, weil der Client die neuen Felder (`revision`, `id`, `change_seq`, `global_revision`)
in den Responses konsumiert. Der 3-Wege-Diff des Clients funktioniert zwar auch ohne sie (über MD5),
aber Lösch-Propagierung vom Server und die Delta-Optimierung brauchen sie.

### B1. Schema-Migration — `server/sql/migrate_003.sql` (neu)

Analog zu `migrate_001/002.sql`. Ergänzt:

- `users.global_revision BIGINT UNSIGNED NOT NULL DEFAULT 0`
  — monotoner **pro-User**-Zähler (jeder User hat sein eigenes Datenset, ADR 04).
- `files.revision INT UNSIGNED NOT NULL DEFAULT 1` — **Inhalts**-Revision (Doc: neu=1, `++` nur bei
  Inhaltsänderung; Rename/Move erhöht NICHT).
- `files.change_seq BIGINT UNSIGNED NOT NULL DEFAULT 0` — Wert von `global_revision` beim letzten
  Change dieser Zeile. Ermöglicht Delta-Abfrage „alles mit `change_seq > N`".
- `files.deleted_at DATETIME NULL DEFAULT NULL` — Soft-Delete-Tombstone (`NULL` = lebendig).
- Index `KEY idx_user_changeseq (user_id, change_seq)` für die Delta-Abfrage.

`schema.sql` (Neuinstallationen) spiegelbildlich um dieselben Spalten erweitern.

### B2. Atomarer globaler Zähler — `server/includes/FileStore.php`

Neue private Helfer, an einer Stelle gekapselt:

```php
// Atomar: erhöht + liefert den neuen Wert (klassischer LAST_INSERT_ID-Trick).
private function bumpGlobalRevision(): int {
    DB::pdo()->prepare(
        'UPDATE users SET global_revision = LAST_INSERT_ID(global_revision + 1) WHERE id = ?'
    )->execute([Auth::$user_id]);
    return (int) DB::pdo()->lastInsertId();
}
private function currentGlobalRevision(): int { /* SELECT global_revision FROM users WHERE id=? */ }
```

PHP-7.4-konform (kein `never`/union). Jeder schreibende Pfad, der einen Change bedeutet, ruft
`bumpGlobalRevision()` **einmal** und schreibt den Rückgabewert in `files.change_seq` der betroffenen Zeile.

### B3. Change-/Revisionslogik in den Endpoints (`FileStore.php`)

- **`upload()`** (`FileStore.php:237`): vorhandene Zeile per `(user_id, path)` selektieren.
  - Neu → `revision = 1`.
  - Existiert, **Inhalt geändert** (`checksum != neuer md5`) → `revision = alt + 1`.
  - Existiert, Inhalt identisch → `revision` unverändert.
  - In **allen drei** Fällen `change_seq = bumpGlobalRevision()`, `deleted_at = NULL`
    (Re-Upload eines gelöschten Pfades belebt ihn wieder).
- **`delete()` → Soft-Delete** (`deleteImpl()`, `FileStore.php:311`): statt `DELETE FROM files`
  jetzt `UPDATE files SET deleted_at = NOW(), change_seq = bumpGlobalRevision()`. Bei `recursive`
  dieselbe Behandlung für die `LIKE 'prefix/%'`-Kinder (jeweils change_seq bumpen). **Datei-Bytes
  auf der Platte werden weiterhin entfernt** (Tombstone ist reine Metadaten-Zeile; Versionierung/
  Blob-Aufbewahrung ist ausdrücklich VCS-Thema, siehe 08/09, nicht dieser Plan).
- **`rename()`** (`FileStore.php:494`): `path` (+ Kinder-Präfixe) umschreiben, `revision`
  **unverändert** (Doc), `change_seq = bumpGlobalRevision()`.
- **`setdate()`** (`FileStore.php:534`): **kein** Bump, **kein** `revision++` — mtime ist laut Doc
  reine Information/Optimierung und entscheidet nie über Änderungen.
- **`list()`/`scanEntries()`** (`FileStore.php:159/196`): Tombstones (`deleted_at IS NOT NULL`)
  standardmäßig **ausblenden**.

### B4. Response-Format erweitern

- `list`/`info`-Einträge (`withMeta()`, `FileStore.php:126`) um `id` (= `files.id`), `revision`,
  `change_seq` ergänzen.
- Neuer Endpoint **`action=changes&since=N`** (`api.php`-Dispatch `:29-57` + `FileStore::changes()`):
  liefert `{ status, global_revision, entries: [...] }`, wobei `entries` **alle** Zeilen mit
  `change_seq > N` sind — **inklusive Tombstones**, jeweils mit `deleted: true/false`. Das ist die
  Delta-Quelle des Clients.
- `status` (`api.php:30-33`) um `global_revision` (des aktuellen Users) ergänzen — erlaubt dem Client
  den billigen „nichts geändert seit N"-Kurzschluss ohne vollen List-Durchlauf.

**Kritische Server-Dateien:** `server/sql/migrate_003.sql` (neu), `server/sql/schema.sql`,
`server/includes/FileStore.php`, `server/api.php`, `docs/api/` (Endpoint-Doku für `changes` + neue Felder).

---

## Phase A (Client) — JSON-Migration auf parson, Last-Sync-DB, 3-Wege-Sync

### A1. JSON-Bibliothek: cJSON → parson (Vollmigration)

Grund: das abgespeckte cJSON in `core/libcloudcore/` kann **nur lesen**, für `.amicloudsync` brauchen
wir aber auch **Schreiben**. Statt cJSON einen Writer aufzupfropfen, ersetzen wir cJSON komplett durch
**parson 1.1.0** (MIT, C89, im Readme explizit für m68k-amigaos paketiert, `AmigaDoku/parson/source/`).
parson kann parse **und** serialize inkl. Datei-I/O und ist damit die einzige JSON-Lib im Projekt.

- **parson in die Lib ziehen**: `parson.c` → `core/libcloudcore/src/`, `parson.h` →
  `core/libcloudcore/include/cloudcore/`. `cJSON.c`/`cJSON.h` aus libcloudcore **entfernen**.
- **Umbrella-Include**: `core/libcloudcore/include/cloudcore/amicloud.h:24` (`#include "cJSON.h"`) auf
  `parson.h` umstellen (oder entfernen und dort includen, wo real gebraucht).
- **Makefiles** (je `Makefile.pc` / `Makefile.amiga` / `Makefile.amiga-cross`): in **libcloudcore**
  `cJSON.o` → `parson.o`; im **handler/cloudhandler** und in Apps, die `core_cJSON.o` referenzieren,
  entsprechend anpassen (die vorhandenen `obj/*/core_cJSON.o` zeigen die betroffenen Ziele).
- **Optional**: `json_set_allocation_functions()` einmal beim Start setzen, falls auf dem Amiga eigene
  Pools gewünscht sind (nicht zwingend).

**Parse-Migration** — nur zwei Dateien nutzen cJSON, rein lesend. API-Abbildung (klein und mechanisch):

| cJSON (heute) | parson (neu) |
|---|---|
| `cJSON_Parse(body)` | `json_parse_string(body)` → `JSON_Value*`; `json_value_get_object(v)` |
| `cJSON_Delete(root)` | `json_value_free(root)` |
| `cJSON_GetObjectItem(o,k)` | `json_object_get_value(o,k)` bzw. typisiert `json_object_get_string/number` |
| `jstr()` (`cmd.c:87`) | `json_object_get_string(o,k)` (const char*, NULL bei Fehlen) |
| `jlong()` (`cmd.c:93`) | `(long)json_object_get_number(o,k)` |
| `cJSON_GetArraySize` / `GetArrayItem` | `json_object_get_array` + `json_array_get_count` / `json_array_get_object` |
| `check_status()` (`cmd.c:113`) | `json_object_get_string(root,"status")` vergleichen |

- `core/libcloudhttp/src/cmd.c`: alle ~18 Parse-Stellen (Liste oben) umstellen; die zwei Helfer
  `jstr`/`jlong` einmal auf parson-Getter umschreiben, der Rest folgt fast automatisch.
- `handler/cloudhandler/src/worker.c`: dieselbe Parse-Umstellung (List-Response).
- **Verhaltensgleich halten**: parson-Getter liefern bei fehlenden Feldern definierte Defaults
  (`get_string`→NULL, `get_number`→0), exakt wie die heutigen `jstr`/`jlong` — Regression-Risiko gering,
  aber alle bestehenden Befehle (list/info/status/upload/…) müssen nachweislich weiter funktionieren
  (siehe Verifikation).

**Kostenpunkt (bekannt/akzeptiert):** parson ist `double`-basiert (`#include <math.h>`, Zahlen via
`sprintf(%1.17g)`, `parson.c:47/1011`) → zieht auf 68000 **Soft-Float** in Client **und** Handler.
Unsere Werte sind Ganzzahlen < 2⁵³ (mtime, size, revision, change_seq) und werden von `%1.17g`
**exakt ohne Exponent** ausgegeben. Das Readme warnt vor Double-Parse-Bugs der mitgelieferten
**clib2**-Testbinary — irrelevant für vbcc, aber im Cross-Build als Verifikationspunkt notieren.

### A2. Neue Felder in `AcEntry` — `core/libcloudcore/include/cloudcore/entry.h`

`AcEntry` (`entry.h:16-25`) um `long id; long revision; long change_seq;` ergänzen (Server liefert sie
jetzt). Parser in `core/libcloudhttp/src/cmd.c` (`cmd_list_raw`/`cmd_info_raw`, ~`:177-275`) füllt die
neuen JSON-Felder; fehlen sie (alter Server), Default 0 → Client fällt automatisch auf reinen
MD5-Vergleich zurück.

### A3. Last-Sync-State-DB — `core/libcloudsync/src/syncstate.c` + `include/cloudsync/syncstate.h` (neu)

Persistente Baseline, als JSON in `<local_root>/.amicloudsync`. Repräsentiert exakt die
Doc-Felder „Lokale Sync-Datenbank":

```c
#define SYNCSTATE_FILE ".amicloudsync"

typedef struct {
    char *path;        /* relativ zur Sync-Wurzel, '/'-verbunden */
    int   is_dir;
    long  size;
    long  mtime;       /* UTC-Epoche */
    char  md5[33];
    long  revision;    /* Server-Revision zum Zeitpunkt des letzten Syncs */
    long  server_id;   /* Server-Datei-ID (optional, 0 = unbekannt) */
    long  last_sync;   /* letzter erfolgreicher Sync-Zeitpunkt (optional) */
} SyncStateEntry;

typedef struct {
    SyncStateEntry *items; int n, cap;
    long global_revision;  /* globale Server-Revision, bis zu der zuletzt gesynct wurde */
} SyncState;

int  syncstate_load(const char *local_root, SyncState *st);   /* fehlt Datei -> leerer State, ok */
int  syncstate_save(const char *local_root, const SyncState *st); /* atomar: .tmp + rename */
SyncStateEntry *syncstate_find(SyncState *st, const char *path);
void syncstate_upsert(SyncState *st, const SyncStateEntry *e);
void syncstate_remove(SyncState *st, const char *path);
void syncstate_free(SyncState *st);
```

- `load` nutzt `json_parse_file()` (parson, A1); `save` baut den DOM mit
  `json_value_init_object`/`json_value_init_array`/`json_object_set_number`/`set_string`/
  `json_array_append_value` und schreibt via `json_serialize_to_file(_pretty)`.
- **Atomar speichern**: erst nach `.amicloudsync.tmp` serialisieren, dann per `rename()` (C-stdlib,
  funktioniert auf PC und AmigaOS/dos) über die Zieldatei — ein abgebrochener Sync darf keine korrupte
  Baseline hinterlassen. (Falls libcloudfs eine eigene Rename-Kapselung braucht, `localfs_rename()`
  ergänzen, statt `rename()` direkt in der Sync-Domäne aufzurufen — FS-Semantik gehört in libcloudfs.)

### A4. `sync.c` auf 3-Wege umbauen — `core/libcloudsync/src/sync.c`

Kern der Änderung. `cloudsync_run()` bekommt drei Eingaben statt zwei: **Remote-Index**,
**Local-Index**, **Baseline** (`SyncState`).

1. **Baseline laden** (`syncstate_load(local_root, &st)`) am Anfang.
2. **Delta-Optimierung**: `status`/`global_revision` vom Server holen. Wenn
   `server_global == st.global_revision`, hat sich remote seit dem letzten Sync **nichts** geändert →
   Remote-Vollscan überspringen und statt `index_remote_tree_sync` `cmd_changes(since=global)` nutzen
   (liefert leere Delta). Andernfalls Remote wie bisher indizieren (oder inkrementell via `changes`).
3. **MD5-Cache** im Local-Index: in `index_local_tree_sync` (`sync.c:115`) pro Datei zuerst
   `syncstate_find(path)`; wenn `size` **und** `mtime` (±2 s, `mtimes_equal`) mit der Baseline
   übereinstimmen → gecachten `md5` übernehmen statt `md5_file_hex()` neu zu rechnen. Nur bei
   Abweichung neu hashen. (Ersetzt das bedingungslose Neu-Hashen in `sync.c:311`.)
4. **`.amicloudsync` vom Scan ausschließen**: in `index_local_tree_sync` auf Wurzelebene
   `strcmp(e.name, SYNCSTATE_FILE)==0` überspringen, sonst lädt der Sync seine eigene DB hoch.
5. **3-Wege-Entscheidungsmatrix** — Merge-Scan über die **Vereinigung von Remote ∪ Local ∪ Baseline**
   (Baseline liefert die Pfade, die auf beiden lebenden Seiten fehlen = echte Löschungen). Pro Pfad mit
   L=local?, R=remote-lebendig?, B=baseline?:

   | L | R | B | Bedingung | Aktion |
   |---|---|---|-----------|--------|
   | ✓ | ✓ | ✓ | keine Seite geändert (md5==B) | nichts (unchanged) |
   | ✓ | ✓ | ✓ | nur local geändert | Upload |
   | ✓ | ✓ | ✓ | nur remote geändert | Download |
   | ✓ | ✓ | ✓ | beide geändert, `Lmd5==Rmd5` | konvergiert → nur Baseline aktualisieren |
   | ✓ | ✓ | ✓ | beide geändert, `Lmd5!=Rmd5` | **Konflikt** (skip, melden) |
   | ✓ | ✓ | ✗ | `Lmd5==Rmd5` | adoptieren, Baseline setzen |
   | ✓ | ✓ | ✗ | `Lmd5!=Rmd5` | **Konflikt** (skip, melden) |
   | ✓ | ✗ | ✓ | local unverändert (==B) | **remote gelöscht → local löschen** |
   | ✓ | ✗ | ✓ | local geändert | **Konflikt** (local behalten, melden) |
   | ✓ | ✗ | ✗ | — | neu local → Upload |
   | ✗ | ✓ | ✓ | remote unverändert (`rev==B.rev`) | **local gelöscht → remote (soft-)löschen** |
   | ✗ | ✓ | ✓ | remote geändert | **Konflikt** (remote behalten, melden) |
   | ✗ | ✓ | ✗ | — | neu remote → Download |
   | ✗ | ✗ | ✓ | — | beidseitig weg → Baseline-Eintrag entfernen |

   „remote geändert" bevorzugt über `revision`/`change_seq` bestimmen (Baseline speichert die
   Revision); MD5 bleibt Fallback bei altem Server. mtime bleibt **nur** Info/Tiebreak (Doc), nie
   allein ausschlaggebend. Verzeichnisse: `SA_MKDIR_*` wie bisher; Lösch-Propagierung erzeugt neue
   Aktionstypen `SA_RMDIR_LOCAL`/`SA_DEL_LOCAL`/`SA_DEL_REMOTE` (letzterer ruft `cmd_del`, das nun
   serverseitig Soft-Delete ist).

6. **Baseline erst nach vollständigem Erfolg schreiben** (Doc-Anforderung): nach Pass 2 die Baseline
   aus dem **konvergierten** Zustand neu aufbauen (pro erfolgreich behandelten Pfad: aktuelle
   `size/mtime/md5/revision/server_id/last_sync=now`), `st.global_revision = server_global`, dann
   `syncstate_save()`. Bei `out->errors > 0` **nur** die erfolgreich abgeschlossenen Pfade
   aktualisieren, die fehlgeschlagenen behalten ihren alten Baseline-Eintrag → nächster Lauf
   wiederholt sie. `dry_run` schreibt **nie** (weder Transfers noch Baseline).

Signatur `cloudsync_run(cfg, local_root, remote_root, dry_run, verbose, Progress*, SyncSummary*)`
bleibt unverändert → Aufrufer (`apps/cloudsync/src/main.c`) müssen nicht angepasst werden.
`SyncSummary` (`sync.h:22`) um `int deleted;` erweitern, damit Löschungen in der Tally-Zeile
sichtbar sind.

### A5. HTTP-Kommandos — `core/libcloudhttp/include/cloudhttp/cmd.h` + `src/cmd.c`

- `cmd_status`/neuer Getter für `global_revision` (aus der `status`-Response).
- **`cmd_changes(cfg, remote_root, long since, AcEntry **out, int *n, long *global_out)`** — ruft
  `action=changes&since=N`, parst Einträge inkl. `deleted`-Flag. Tombstones als eigenes Feld in
  `AcEntry` abbilden (`int deleted;`) oder über eine Parallel-Liste. URL-Bau bleibt in `cmd.c`
  (`cmd_resolve_path`), Kapselungsregel.

### A6. Doku

`docs/architecture/07_Sync_Metadaten.md` an die final gewählten Feldnamen angleichen
(`change_seq`, `.amicloudsync`, per-User globale Revision, Soft-Delete). Kurzer Verweis auf den neuen
`changes`-Endpoint. Kein Umschreiben, nur Präzisierung.

**Kritische Client-Dateien:** `core/libcloudcore/src/parson.c` + `include/cloudcore/parson.h` (aus
`AmigaDoku/parson/source/` übernommen; `cJSON.c`/`cJSON.h` entfernt),
`core/libcloudcore/include/cloudcore/amicloud.h` (Include-Umstellung),
`core/libcloudcore/include/cloudcore/entry.h` (neue `AcEntry`-Felder),
`core/libcloudsync/src/syncstate.c` + `include/cloudsync/syncstate.h` (neu),
`core/libcloudsync/src/sync.c`, `core/libcloudsync/include/cloudsync/sync.h`,
`core/libcloudhttp/src/cmd.c` (Parse-Migration + `cmd_changes`) + `include/cloudhttp/cmd.h`,
`handler/cloudhandler/src/worker.c` (Parse-Migration). Makefiles je `Makefile.pc` /
`Makefile.amiga` / `Makefile.amiga-cross` von libcloudcore, dem Handler und allen Apps mit
`core_cJSON.o`: `cJSON.o` → `parson.o`, neue Sync-Quellen ergänzen.

---

## Verifikation

**Server (Test-DB):**
1. `migrate_003.sql` einspielen. `curl` gegen `api.php`:
   - Upload neue Datei → Response enthält `revision=1`; `users.global_revision` +1, `files.change_seq`
     = dieser Wert.
   - Upload **anderer** Inhalt gleicher Pfad → `revision=2`, global +1.
   - Upload **gleicher** Inhalt → `revision` unverändert, global +1.
   - `action=delete` → `deleted_at` gesetzt, Bytes weg, `list` zeigt Pfad **nicht** mehr,
     `changes?since=0` zeigt ihn **mit** `deleted:true`.
   - `action=rename` → `revision` unverändert, `change_seq` erhöht, `changes` zeigt neuen Pfad.
   - `action=setdate` → **kein** global-Bump, `revision` unverändert.
   - `status` liefert aktuelles `global_revision`.

**parson-Migration (Regression, PC-Build):** Zuerst isoliert prüfen, dass die JSON-Umstellung nichts
kaputt macht — **alle** bestehenden Befehle gegen den Server müssen weiter korrekt parsen:
`list`, `info`, `status`, `upload`, `download`, `delete`, `mkdir`, `rename` (CLI `amicloud` +
`amicloud-shell`). Erwartete Felder (Name/Größe/mtime/md5/protection/comment) müssen unverändert
ankommen; Handler (`worker.c`) an einem echten Mount gegentesten. Erst danach die Sync-Schritte.

**Client (PC-Build):** Libs + `apps/cloudsync` per `Makefile.pc` bauen. Skript gegen den
Test-Server, Baseline-Datei nach jedem Schritt inspizieren:
1. Leerer Start → Sync legt `.amicloudsync` an, `global_revision` gesetzt.
2. Lokale Datei anlegen → Sync: Upload, Baseline-Eintrag `revision=1`.
3. Remote ändern (2. Client/curl) → Sync: Download, Baseline-Revision steigt.
4. **Lokal löschen** → Sync: remote Soft-Delete (Tombstone prüfen), nicht Re-Download. *(Der heutige
   Code würde die Datei fälschlich wieder herunterladen — Kern-Regression-Check.)*
5. **Remote löschen** → Sync: lokal gelöscht.
6. **Beidseitig unterschiedlich editieren** → **Konflikt gemeldet, nichts überschrieben** (beide
   Versionen intakt).
7. **MD5-Cache**: unveränderte Datei über `HLOG`/Debug-Zähler prüfen, dass `md5_file_hex` **nicht**
   erneut läuft.
8. **Delta-Kurzschluss**: zweiter Sync ohne Änderungen ruft `changes` mit leerem Ergebnis, kein
   Remote-Vollscan.
9. `.amicloudsync` taucht **nie** in Transfers/Server-Listing auf.

**Cross-Compile (vbcc `+aos68k`):** **alle** Ziele bauen (nicht nur cloudsync — die parson-Migration
berührt libcloudcore, libcloudhttp, den Handler und jede App). Verifiziert, dass `parson.c` und
`syncstate.c` C89-/m68k-sauber übersetzen und dass das per parson reingezogene **Soft-Float**
(`math.h`, `%1.17g`) sauber linkt (Achtung auf die im Readme erwähnten Double-Parse-Bugs — hier
konkret prüfen, dass eine geschriebene und wieder gelesene Baseline mit großen `mtime`/`change_seq`
bit-genau round-trippt). Kein Laufzeittest auf echter Hardware nötig, aber Übersetzen + Linken aller
Ziele muss sauber durchgehen.
