# Konfigurationsbeispiele (AmiOwnCloud)

Ein Beispiel pro Konfigurationsformat, das AmiOwnCloud verwendet. `make
dist-amiga` legt alle Dateien dieses Ordners in die Unterschublade
`Examples/` der Amiga-Installationsschublade.

| Datei | Ablage im Betrieb | Gelesen von | Geschrieben von |
|---|---|---|---|
| [amiowncloud.cfg.example](amiowncloud.cfg.example) | `ENV:amiowncloud.cfg` (aktiv) / `ENVARC:amiowncloud.cfg` (dauerhaft) | `amiowncloud-worker` (Backend des `OWNCLOUD:`-Handlers, liest nur `ENV:` und nur beim Mounten) | AmiOwnCloudPrefs (`apps/amiowncloud-prefs`) |
| [OWNCLOUD.mountlist](OWNCLOUD.mountlist) | beliebig, z. B. an `DEVS:MountList` anhängen | AmigaDOS `Mount OWNCLOUD: FROM …` (`handler/owncloudhandler`) | -- (von Hand) |
| [AmiOwnCloudSync.cfg.example](AmiOwnCloudSync.cfg.example) | `ENV:AmiOwnCloudSync.cfg` / `ENVARC:AmiOwnCloudSync.cfg` (ENV: hat Vorrang, kein weiterer Ort) | Commodity `AmiOwnCloudSync` (`apps/amiowncloud-sync-cx`, nur lesend) | AmiOwnCloudSyncPrefs (`apps/amiowncloud-sync-prefs`) |
| [amiowncloud-sync.cfg.example](amiowncloud-sync.cfg.example) | Amiga: `ENV:amiowncloud-sync.cfg`, ersatzweise `amiowncloud-sync.cfg` im aktuellen Verzeichnis; macOS/Linux: `~/.amiowncloud-syncrc`; Windows (MinGW): `amiowncloud-sync.cfg` im aktuellen Verzeichnis; oder `-c <datei>` | `amiowncloud-sync`, `amiowncloud-synctest` | -- (von Hand) |

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
| `basepath` | Nur `amiowncloud.cfg`: Unterordner der Sammlung, der als Wurzel von `OWNCLOUD:` erscheint | `/` |
| `verify_ssl` | TLS-Zertifikat prüfen (`1`) oder nicht (`0`, nur für Tests) | `1` |
| `debug` | Nur Amiga, nur `amiowncloud.cfg` und `AmiOwnCloudSync.cfg`: Datei für das HTTP-Protokoll (z. B. `T:amiowncloud.log`) | leer = aus |
| `gmt_offset` | Nur `amiowncloud.cfg`: Abstand Ortszeit zu UTC in Minuten (MEZ = `60`) | `0` |
| `dst` | Nur `amiowncloud.cfg`: Sommerzeit aktiv (`1`) oder nicht (`0`) | `0` |
| `timeout` | Netz-Timeout in Sekunden: so lange darf eine Verbindung ganz ohne Datenfluss bleiben; nie eine Grenze für eine ganze Übertragung (Modem!) | `120` |
| `cache_ttl` | Nur `amiowncloud.cfg`: Lebensdauer der Verzeichnislisten von `OWNCLOUD:` in Sekunden; über ein Modem lohnen 120–300 | `30` |

`AmiOwnCloudSync.cfg` kennt zusätzlich:

| Schlüssel | Bedeutung | Standard |
|---|---|---|
| `interval` | Minuten zwischen zwei automatischen Abgleichen, `0` oder negativ = aus | `15` |
| `pair` | Ordnerpaar `lokal\|remote\|on` bzw. `off` (auch `1`/`0`), beliebig oft | – |

## Prefs-Editoren

AmiOwnCloudPrefs und AmiOwnCloudSyncPrefs folgen der klassischen
Workbench-Prefs-Semantik: **Benutzen** schreibt nur nach `ENV:`,
**Speichern** nach `ENVARC:` *und* `ENV:`, **Abbrechen** schreibt nichts.
Beide schreiben die Datei komplett neu – Kommentare gehen dabei verloren.
AmiOwnCloudSyncPrefs schreibt außerdem keine `debug=`-Zeile; wer das
Debug-Log der Commodity braucht, trägt sie nach dem Speichern von Hand
wieder ein.

Jede Datei ist ein eigenständiges Profil: `amiowncloud.cfg`,
`AmiOwnCloudSync.cfg` und `amiowncloud-sync.cfg` teilen sich keine Zugangsdaten,
auch wenn sie auf denselben Server zeigen.
