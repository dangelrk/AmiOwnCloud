# Konfigurationsbeispiele (AmiOwnCloud)

Ein Beispiel pro Konfigurationsformat, das AmiOwnCloud verwendet. `make
dist-amiga` legt alle Dateien dieses Ordners in die Unterschublade
`Examples/` der Amiga-Installationsschublade.

| Datei | Ablage im Betrieb | Gelesen von | Geschrieben von |
|---|---|---|---|
| [owncloud.cfg.example](owncloud.cfg.example) | `ENV:owncloud.cfg` (aktiv) / `ENVARC:owncloud.cfg` (dauerhaft) | `owncloud-worker` (Backend des `OWNCLOUD:`-Handlers, liest nur `ENV:` und nur beim Mounten) | OwnCloudPrefs (`apps/owncloud-prefs`) |
| [OWNCLOUD.mountlist](OWNCLOUD.mountlist) | `DEVS:DOSDrivers/OWNCLOUD` (Mount beim Booten) oder `SYS:Storage/DOSDrivers/OWNCLOUD` (Mount von Hand) | `Mount` / DOS (`handler/owncloudhandler`) | -- (von Hand) |
| [AmiCloudSync.cfg.example](AmiCloudSync.cfg.example) | `ENV:AmiCloudSync.cfg` / `ENVARC:AmiCloudSync.cfg` (ENV: hat Vorrang, kein weiterer Ort) | Commodity `AmiCloudSync` (`apps/amicloud-sync-cx`, nur lesend) | AmiCloudSyncPrefs (`apps/amicloudsync-prefs`) |
| [webdavsync.cfg.example](webdavsync.cfg.example) | Amiga: `ENV:webdavsync.cfg`, ersatzweise `webdavsync.cfg` im aktuellen Verzeichnis; macOS/Linux: `~/.webdavsyncrc`; Windows (MinGW): `webdavsync.cfg` im aktuellen Verzeichnis; oder `-c <datei>` | `webdavsync`, `webdavsynctest` | -- (von Hand) |

`OWNCLOUD.mountlist` ist eine inhaltsgleiche Kopie von
`handler/owncloudhandler/mount/OWNCLOUD` (nur die Kopfzeile des Kommentars
unterscheidet sich). Änderungen bitte an beiden Stellen nachziehen.

## Gemeinsames Format

Alle `.cfg`-Dateien verwenden denselben Parser (`core/libcloudcore/src/config.c`):
Zeilen `schluessel=wert`, Leerzeichen um `=` sind erlaubt, Zeilen mit `#`
am Anfang sind Kommentare. Bekannte Schlüssel:

| Schlüssel | Bedeutung | Standard |
|---|---|---|
| `server` | Hauptserver (`https://cloud.example.com`) oder volle WebDAV-Sammel-URL (`…/remote.php/dav/files/<user>`). Aus dem Hauptserver wird `<server>/remote.php/dav/files/<user>` gebildet. | – |
| `user` / `username` | Benutzername | – |
| `pass` / `password` | Passwort, am besten ein App-Passwort des Servers | – |
| `basepath` | Nur `owncloud.cfg`: Unterordner der Sammlung, der als Wurzel von `OWNCLOUD:` erscheint | `/` |
| `verify_ssl` | TLS-Zertifikat prüfen (`1`) oder nicht (`0`, nur für Tests) | `1` |
| `debug` | Nur Amiga, nur `owncloud.cfg` und `AmiCloudSync.cfg`: Datei für das HTTP-Protokoll (z. B. `T:owncloud.log`) | leer = aus |
| `gmt_offset` | Nur `owncloud.cfg`: Abstand Ortszeit zu UTC in Minuten (MEZ = `60`) | `0` |
| `dst` | Nur `owncloud.cfg`: Sommerzeit aktiv (`1`) oder nicht (`0`) | `0` |

`AmiCloudSync.cfg` kennt zusätzlich:

| Schlüssel | Bedeutung | Standard |
|---|---|---|
| `interval` | Minuten zwischen zwei automatischen Abgleichen, `0` oder negativ = aus | `15` |
| `pair` | Ordnerpaar `lokal\|remote\|on` bzw. `off` (auch `1`/`0`), beliebig oft | – |

## Prefs-Editoren

OwnCloudPrefs und AmiCloudSyncPrefs folgen der klassischen
Workbench-Prefs-Semantik: **Benutzen** schreibt nur nach `ENV:`,
**Speichern** nach `ENVARC:` *und* `ENV:`, **Abbrechen** schreibt nichts.
Beide schreiben die Datei komplett neu – Kommentare gehen dabei verloren.
AmiCloudSyncPrefs schreibt außerdem keine `debug=`-Zeile; wer das
Debug-Log der Commodity braucht, trägt sie nach dem Speichern von Hand
wieder ein.

Jede Datei ist ein eigenständiges Profil: `owncloud.cfg`,
`AmiCloudSync.cfg` und `webdavsync.cfg` teilen sich keine Zugangsdaten,
auch wenn sie auf denselben Server zeigen.
