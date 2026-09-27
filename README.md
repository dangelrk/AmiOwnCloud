# AmiOwnCloud

*Connects AmigaOS 3.x to a standard ownCloud/Nextcloud server via WebDAV:
an `OWNCLOUD:` DOS handler that mounts the server as a drive, a background
folder-sync commodity (AmiCloudSync) with GadTools preferences editors, and
the portable command-line tools `webdavsync`/`webdavsynctest` for AmigaOS,
macOS, Linux and Windows. Written in C99, built with vbcc for the Amiga.*

AmiOwnCloud bindet einen vorhandenen ownCloud- oder Nextcloud-Server an
AmigaOS 3.x an – über WebDAV, dieselbe Schnittstelle, die auch die
offiziellen Clients benutzen. Ein eigener Server gehört nicht dazu; man
braucht nur ein Konto auf einem ownCloud/Nextcloud-Server.

Zwei unabhängige Wege, die sich auch kombinieren lassen:

- **`OWNCLOUD:`-Handler** – der Server als Laufwerk. Jeder Zugriff geht
  live an den Server, keine lokale Kopie.
- **AmiCloudSync** – eine Commodity, die lokale Amiga-Ordner mit Ordnern auf
  dem Server abgleicht (automatisch im Intervall oder auf Knopfdruck); die
  Dateien sind auch offline da.

## Komponenten

| Programm | Plattform | Zweck | Quellen |
|---|---|---|---|
| `owncloud-handler` + `owncloud-worker` | Amiga | DOS-Handler `OWNCLOUD:` (Volume `OwnCloud:`), WebDAV live | `handler/owncloudhandler` |
| `OwnCloudPrefs` | Amiga | Einstellungs-Editor für `owncloud.cfg` (Handler) | `apps/owncloud-prefs` |
| `AmiCloudSync` | Amiga | Commodity: Ordnerpaare im Hintergrund abgleichen, Fenster per Hotkey | `apps/amicloud-sync-cx` |
| `AmiCloudSyncPrefs` | Amiga | Einstellungs-Editor für Profil und Ordnerpaare (`AmiCloudSync.cfg`) | `apps/amicloudsync-prefs` |
| `webdavsync` | Amiga + PC | einmaliger Zwei-Wege-Abgleich eines Ordnerpaares per Kommandozeile | `apps/webdavsync` |
| `webdavsynctest` | Amiga + PC | automatische Testsuite (14 Prüfungen) gegen einen echten Server | `apps/webdavsynctest` |

„PC“ heißt macOS, Linux und Windows; getestet ist bisher macOS.

## Schnellstart

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiOwnCloud.git
cd AmiOwnCloud

make pc                          # webdavsync, webdavsynctest -> bin/pc/

export VBCC=/pfad/zu/vbcc
make amiga                       # alle Amiga-Programme -> bin/amiga/
make dist-amiga                  # Installationsschublade build/dist/AmiOwnCloud/
```

Auf dem Amiga dann in der Schublade `Execute Install-AmiOwnCloud`.

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

## Verzeichnisse

```
apps/             Programme (je src/, Makefile.pc und/oder Makefile.amiga-cross)
handler/          OWNCLOUD:-DOS-Handler (nur Amiga)
config-examples/  Beispielkonfigurationen und Mountlist
install/amiga/    Install-/Uninstall-Skript und ReadMe der Amiga-Schublade
docs/             Dokumentation
scripts/          Hilfsskripte (Host-Test, FS-UAE-Start)
core/             AmiCloudCore (Git-Submodul)
Build-Amiga       erzeugtes AmigaDOS-Skript zum nativen Bauen
```

## Verwandte Projekte

AmiOwnCloud nutzt die gemeinsamen Bibliotheken aus
[AmiCloudCore](https://github.com/dangelrk/AmiCloudCore) (Konfiguration,
HTTP/TLS über bsdsocket + AmiSSL bzw. libcurl, lokales Dateisystem,
Sync-Kern), eingebunden als Submodul `core/`. Schwesterprojekte:

- [AmiCloud](https://github.com/dangelrk/AmiCloud) – eigener PHP-Server mit
  CLI, Shell, Commander und `CLOUD:`-Handler (unabhängig von AmiOwnCloud)
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
