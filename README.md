# AmiOwnCloud

*Connects AmigaOS 3.x to a standard ownCloud/Nextcloud server via WebDAV:
an `OWNCLOUD:` DOS handler that mounts the server as a drive, a background
folder-sync commodity (AmiOwnCloudSync) with GadTools preferences editors, and
the portable command-line tools `amiowncloud-sync`/`amiowncloud-synctest` for AmigaOS,
macOS, Linux and Windows. Written in C99, built with vbcc for the Amiga.*

> **Status:** experimental. Developed with the help of Claude (an AI assistant
> by Anthropic), primarily for the author's own use. Try it with copies of
> your data first; use at your own risk.

AmiOwnCloud bindet einen vorhandenen ownCloud- oder Nextcloud-Server an
AmigaOS 3.x an – über WebDAV, dieselbe Schnittstelle, die auch die
offiziellen Clients benutzen. Ein eigener Server gehört nicht dazu; man
braucht nur ein Konto auf einem ownCloud/Nextcloud-Server.

Zwei unabhängige Wege, die sich auch kombinieren lassen:

- **`OWNCLOUD:`-Handler** – der Server als Laufwerk. Jeder Zugriff geht
  live an den Server, keine lokale Kopie.
- **AmiOwnCloudSync** – eine Commodity, die lokale Amiga-Ordner mit Ordnern auf
  dem Server abgleicht (automatisch im Intervall oder auf Knopfdruck); die
  Dateien sind auch offline da.

## Komponenten

| Programm | Plattform | Zweck | Quellen |
|---|---|---|---|
| `amiowncloud-handler` + `amiowncloud-worker` | Amiga | DOS-Handler `OWNCLOUD:` (Volume `OwnCloud:`), WebDAV live | `handler/owncloudhandler` |
| `AmiOwnCloudPrefs` | Amiga | Einstellungs-Editor für `amiowncloud.cfg` (Handler) | `apps/amiowncloud-prefs` |
| `AmiOwnCloudSync` | Amiga | Commodity: Ordnerpaare im Hintergrund abgleichen, Fenster per Hotkey | `apps/amiowncloud-sync-cx` |
| `AmiOwnCloudSyncPrefs` | Amiga | Einstellungs-Editor für Profil und Ordnerpaare (`AmiOwnCloudSync.cfg`) | `apps/amiowncloud-sync-prefs` |
| `amiowncloud-sync` | Amiga + PC | einmaliger Zwei-Wege-Abgleich eines Ordnerpaares per Kommandozeile | `apps/amiowncloud-sync` |
| `amiowncloud-synctest` | Amiga + PC | automatische Testsuite (14 Prüfungen) gegen einen echten Server | `apps/amiowncloud-synctest` |

„PC“ heißt macOS, Linux und Windows; getestet ist bisher macOS.

## Schnellstart

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiOwnCloud.git
cd AmiOwnCloud

make pc                          # amiowncloud-sync, amiowncloud-synctest -> bin/pc/

export VBCC=/pfad/zu/vbcc
make amiga                       # alle Amiga-Programme -> bin/amiga/
make dist-amiga                  # Installationsschublade build/dist/AmiOwnCloud/
make release                     # build/release/AmiOwnCloud-<version>.lha + .readme
```

Auf dem Amiga dann in der Schublade das Icon `Install-AmiOwnCloud` doppelt
anklicken (Commodore-Installer; ohne Installer `Execute Install-AmiOwnCloud.shell`).

## Dokumentation

| Dokument | Inhalt |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Installation auf Amiga und PC, Konfigurationsorte, Deinstallation |
| [docs/BUILD.md](docs/BUILD.md) | Bauen: vbcc-Cross-Compile (macOS/Linux/Windows), nativ mit `Build-Amiga`, PC-Build, Tests, Emulator |
| [docs/user/AmiOwnCloud.guide](docs/user/AmiOwnCloud.guide) | Handbuch als AmigaGuide (ISO-8859-1) |
| [docs/user/AmiOwnCloud-Handbuch.html](docs/user/AmiOwnCloud-Handbuch.html) | dasselbe Handbuch als HTML |
| [config-examples/README.md](config-examples/README.md) | alle Konfigurationsdateien und Schlüssel |
| [handler/owncloudhandler/README.md](handler/owncloudhandler/README.md) | Technik des `OWNCLOUD:`-Handlers |
| [docs/design/](docs/design/README.md) | historische Planungsdokumente |
| [TODO.md](TODO.md) | bekannte offene Punkte |

## Verzeichnisse

```
apps/             Programme (je src/, Makefile.pc und/oder Makefile.amiga-cross)
handler/          OWNCLOUD:-DOS-Handler (nur Amiga)
config-examples/  Beispielkonfigurationen und Mountlist
install/amiga/    Install-/Uninstall-Skript und ReadMe der Amiga-Schublade
docs/             Dokumentation
scripts/          Hilfsskript für den Host-Test (test_pairs.sh)
core/             AmiCloudCore (Git-Submodul)
Build-Amiga       erzeugtes AmigaDOS-Skript zum nativen Bauen
```

## Verwandte Projekte

AmiOwnCloud nutzt die gemeinsamen Bibliotheken aus
[AmiCloudCore](https://github.com/dangelrk/AmiCloudCore) (Konfiguration,
HTTP/TLS über bsdsocket + AmiSSL bzw. libcurl, lokales Dateisystem,
Sync-Kern), eingebunden als Submodul `core/`. Schwesterprojekte:

- [AmiCloud](https://github.com/dangelrk/AmiCloud) – eigener PHP-Server mit
  CLI, Shell, Commander und `AMICLOUD:`-Handler (unabhängig von AmiOwnCloud)
- [AmiVCS](https://github.com/dangelrk/AmiVCS) – Versionsverwaltung für den
  Amiga

## Lizenz

Copyright (C) 2026 Ronny Krueger

AmiOwnCloud ist freie Software unter der **GNU General Public License
Version 3 oder neuer** (GPL-3.0-or-later), siehe [LICENSE](LICENSE). Der in
AmiCloudCore enthaltene JSON-Parser Parson steht unter der MIT-Lizenz (siehe
dort `THIRD-PARTY-NOTICES.md`).

ownCloud und Nextcloud sind Marken ihrer jeweiligen Inhaber; AmiOwnCloud ist
ein unabhängiges Projekt.
