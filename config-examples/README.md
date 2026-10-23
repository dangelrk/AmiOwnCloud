# Konfigurationsbeispiele (AmiOwnCloud)

Ein Beispiel pro Konfigurationsformat, das AmiOwnCloud verwendet. `make
dist-amiga` legt alle Dateien dieses Ordners in die Unterschublade
`Examples/` der Amiga-Installationsschublade.

| Datei | Ablage im Betrieb | Gelesen von | Geschrieben von |
|---|---|---|---|
| [owncloud.cfg.example](owncloud.cfg.example) | `ENV:owncloud.cfg` (aktiv) / `ENVARC:owncloud.cfg` (dauerhaft) | `owncloud-worker` (Backend des `OWNCLOUD:`-Handlers, liest nur `ENV:` und nur beim Mounten) | OwnCloudPrefs (`apps/owncloud-prefs`) |
| [OWNCLOUD.mountlist](OWNCLOUD.mountlist) | beliebig, z. B. an `DEVS:MountList` anhängen | AmigaDOS `Mount OWNCLOUD: FROM …` (`handler/owncloudhandler`) | -- (von Hand) |
| [AmiCloudSync.cfg.example](AmiCloudSync.cfg.example) | `ENV:AmiCloudSync.cfg` / `ENVARC:AmiCloudSync.cfg` (ENV: hat Vorrang, kein weiterer Ort) | Commodity `AmiCloudSync` (`apps/amicloud-sync-cx`, nur lesend) | AmiCloudSyncPrefs (`apps/amicloudsync-prefs`) |
| [webdavsync.cfg.example](webdavsync.cfg.example) | Amiga: `ENV:webdavsync.cfg`, ersatzweise `webdavsync.cfg` im aktuellen Verzeichnis; macOS/Linux: `~/.webdavsyncrc`; Windows (MinGW): `webdavsync.cfg` im aktuellen Verzeichnis; oder `-c <datei>` | `webdavsync`, `webdavsynctest` | -- (von Hand) |

`OWNCLOUD.mountlist` ist ein klassischer Mountlist-Eintrag (`OWNCLOUD:` …
`#`) mit denselben Werten wie der DOSDriver
`handler/owncloudhandler/mount/OWNCLOUD`. Den DOSDriver legt
`make dist-amiga` als `Storage/DOSDrivers/OWNCLOUD` in die
Installationsschublade; das Install-Skript kopiert ihn nach
`DEVS:DOSDrivers/` oder `SYS:Storage/DOSDrivers/`. Die Mountlist ist die
Alternative ohne DOSDriver: `Mount OWNCLOUD: FROM Examples/OWNCLOUD.mountlist`.
Änderungen an den Werten bitte an beiden Stellen nachziehen.

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
| `timeout` | Netz-Timeout in Sekunden: so lange darf eine Verbindung ganz ohne Datenfluss bleiben; nie eine Grenze für eine ganze Übertragung (Modem!) | `120` |
| `cache_ttl` | Nur `owncloud.cfg`: Lebensdauer der Verzeichnislisten von `OWNCLOUD:` in Sekunden; über ein Modem lohnen 120–300 | `30` |

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
