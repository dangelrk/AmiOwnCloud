# OWNCLOUD: – ownCloud/Nextcloud-(WebDAV-)DOS-Handler

Ein DOS-Handler für AmigaOS 3.x, der einen normalen ownCloud- oder
Nextcloud-Server als Laufwerk `OWNCLOUD:` einbindet (zusätzlich erscheint
ein Workbench-Volume `OwnCloud:`). Jede Operation (`Dir`, `Copy`, `Delete`,
`Rename`, …) geht live per WebDAV an den Server – es gibt keine lokale
Kopie und keinen Hintergrundabgleich (dafür ist die Commodity
AmiOwnCloudSync da, siehe `docs/user/AmiOwnCloud.guide`).

Nur für AmigaOS. Endnutzer-Doku: Kapitel „OWNCLOUD:-Handler“ in
`docs/user/AmiOwnCloud.guide` bzw. `docs/user/AmiOwnCloud-Handbuch.html`.
Diese Datei beschreibt Aufbau, Bau und Test.

## Herkunft und Aufbau

Der Handler ist ein eigenständiger Fork des `AMICLOUD:`-Handlers aus dem
Schwesterprojekt [AmiCloud](https://github.com/dangelrk/AmiCloud)
(`handler/cloudhandler/`, siehe dessen README.md). Architektur,
Packet-Schleife und Drahtprotokoll zwischen den beiden Prozessen sind
identisch; dort ist ausführlich beschrieben, warum der Handler
`-nostdlib` gelinkt wird, warum er aus zwei Prozessen besteht (Vermeidung
eines Guru `0x87000004`), wie der Verzeichnis-Cache funktioniert und wem
welche Puffer gehören. Das wird hier nicht wiederholt.

Zwei Prozesse:

- **`amiowncloud-handler`** (`L:`) – der eigentliche DOS-Handler. Nutzt nur
  exec.library in der Packet-Schleife, kein Netzwerk, keine libc.
- **`amiowncloud-worker`** (`L:`) – ein normales AmigaOS-Programm
  (vbcc-Startup, libc, bsdsocket.library, AmiSSL 5), das sämtliche
  HTTP/TLS-Arbeit erledigt. Der Handler sucht beim Mounten den
  öffentlichen Port `AmiOwnCloud-Worker` und startet `L:amiowncloud-worker`,
  falls er noch nicht läuft. Die Netzwerk-Initialisierung passiert erst
  bei der ersten echten Anfrage, damit ein Mount beim Booten (`Activate =
  1`) nicht auf den TCP/IP-Stack warten muss.

Unterschiede zum `AMICLOUD:`-Handler:

- **Backend**: `src/worker.c` verwendet `webdavcmd.c` aus
  `core/libcloudhttp` (WebDAV/RFC 4918: PROPFIND/GET/PUT/DELETE/MKCOL/
  MOVE/PROPPATCH) statt der AmiCloud-REST-API. Warum nicht der
  WebDAV-Transport aus `core/libcloudsync/src/webdav.c` (den AmiOwnCloudSync
  und amiowncloud-sync benutzen): der ist dateibasiert, der Handler hält
  Dateiinhalte pro offener Datei im RAM – siehe
  `core/libcloudhttp/include/cloudhttp/webdavcmd.h`.
- **Konfiguration**: `ENV:amiowncloud.cfg` (Beispiel:
  `config-examples/amiowncloud.cfg.example`, Editor: AmiOwnCloudPrefs). Gleiches
  Format wie alle `.cfg`-Dateien (`server`/`user`/`pass`/`basepath`/
  `verify_ssl`/`debug`/`gmt_offset`/`dst`); `server` ist der Hauptserver
  oder die volle WebDAV-Sammel-URL.
- **Namen**: Gerät `OWNCLOUD:`, Volume `OwnCloud:`, Worker-Port
  `AmiOwnCloud-Worker`, Worker-Programm `L:amiowncloud-worker`
  (`src/proto.h`, `src/handler.c`). Ein laufender `AMICLOUD:`-Handler von
  AmiCloud und dieser Handler kommen sich nicht in die Quere.
- **`Protect`/`FileNote` werden angenommen, aber nicht gespeichert.**
  WebDAV hat keine Entsprechung für Amiga-Schutzbits oder Dateikommentare.
  Damit Workbench-Kopieren und ähnliche Werkzeuge nicht scheitern,
  beantwortet der Worker diese Anfragen sofort mit Erfolg, ohne den Server
  zu fragen. Ein folgendes `List` zeigt den Wert nicht.
- **Keine MD5-Prüfung** bei Up- und Download (WebDAV liefert keinen
  Inhalts-Hash).
- Felder ohne WebDAV-Quelle (Schutzbits, Kommentar, MD5, Server-IDs) sind
  immer 0 bzw. leer.

## Bekannte Einschränkungen

- **Lesen holt nur Teile, Ändern die ganze Datei.** Öffnen überträgt
  nichts; jedes Read holt per `Range` nur, was es braucht (`.info` und
  Dateien bis 4 KB ganz, sonst 4 KB beim ersten Zugriff, dann wachsende
  Stücke bis 1 MB). So kostet eine Datei ohne Icon bei „Alle Dateien
  anzeigen“ nur 4 KB. Erst Write/SetFileSize laden die ganze Datei ins
  RAM; sie wird beim Schließen hochgeladen.
- **Eine Server-Anfrage zur Zeit.** Während eine HTTP-Anfrage läuft,
  beantwortet der Handler weiter alles, was ohne Netz geht (`Info`,
  Lesen/Schreiben/Seek auf offenen Dateien, `Examine` frisch gelesener
  Verzeichnisse, `FreeLock`; `src/handler.c` `serve_while_busy()`); alles
  andere wartet und läuft danach in Ankunftsreihenfolge.
- **Kein Zeitlimit im Handler, aber im Worker:** die HTTP-Schicht bricht
  nur bei Stillstand ab (`timeout=` in `amiowncloud.cfg`, Standard 120 s),
  nie wegen der Dauer einer Übertragung. Transportfehler meldet der
  Handler als `ERROR_NO_DISK` (DOS bietet „Wiederholen“ an);
  abgebrochene Downloads setzt die HTTP-Schicht per `Range` fort.
  `Break` auf `amiowncloud-worker` bricht die laufende Übertragung ab. Der
  frühere 60-s-Timer (`timer.device`) wurde entfernt: bei Modemtempo
  ist er schon für 200 KB zu kurz und legte den Mount dauerhaft lahm.
- **Fehlgeschlagenes Hochladen beim Schließen:** `Close()` meldet den
  Fehler, der Handler gibt Datei und Puffer trotzdem frei (DOS gibt den
  FileHandle ohnehin frei); der Worker legt den Inhalt vorher unter
  `T:AmiOwnCloud-unsaved/<name>` ab.
- **Kein Unmount.** `ACTION_DIE` wird immer abgelehnt; der Worker wird nie
  beendet. Aushängen = neu booten.
- **Konfigurationsänderungen erst nach einem Neustart.** `amiowncloud-worker`
  liest `ENV:amiowncloud.cfg` nur einmal beim eigenen Start. Da er weiterläuft
  und der Handler nicht ausgehängt werden kann, gilt eine geänderte
  Konfiguration zuverlässig erst nach einem Reboot.
- `Info OWNCLOUD:` zeigt eine feste Platzhalter-Kapazität (ca. 2 GB, halb
  voll), nicht die echte Belegung auf dem Server.
- Kein rekursives Löschen im Handler: ein nicht leeres Verzeichnis liefert
  „Verzeichnis nicht leer“; `Delete … ALL` erledigt die Rekursion selbst.
- `SetDate` wird per PROPPATCH weitergereicht; ein WebDAV-Server ohne die
  ownCloud/Nextcloud-Erweiterung dafür ignoriert es stillschweigend.
- Ein Volume, keine Disk-Wechsel-/Format-/Inhibit-Pakete;
  `ACTION_EXAMINE_ALL` ist nicht implementiert (DOS weicht automatisch auf
  `EXAMINE_OBJECT`/`EXAMINE_NEXT` aus).
- Verzeichnislisten werden 30 Sekunden zwischengespeichert
  (`CACHE_TTL` in `src/vol.h`, per `cache_ttl=` in `amiowncloud.cfg`
  einstellbar); Änderungen, die ein anderes Gerät am Server macht,
  erscheinen entsprechend verzögert. Lässt sich ein noch nie gelistetes
  Verzeichnis nicht lesen, meldet der Handler den Fehler, statt es leer
  erscheinen zu lassen.

## Bauen

Cross-Compile von macOS/Linux/Windows mit vbcc (Details und
Voraussetzungen: `docs/BUILD.md`):

```sh
export VBCC=/pfad/zu/vbcc
make -C handler/owncloudhandler -f Makefile.amiga-cross
# oder alles zusammen aus der Projektwurzel:  make amiga
```

Ergebnis: `bin/amiga/amiowncloud-handler` und `bin/amiga/amiowncloud-worker`.

Nativ auf dem Amiga: in der Projektwurzel `Execute Build-Amiga` (baut alle
Programme, siehe `docs/BUILD.md`).

## Installieren

Am einfachsten über die Installationsschublade (`make dist-amiga`,
darin `Execute Install-AmiOwnCloud`, siehe `docs/INSTALL.md`). Von Hand:

```
Copy amiowncloud-handler L:
Copy amiowncloud-worker  L:
Copy OWNCLOUD DEVS:DOSDrivers/      ; Datei aus mount/
AmiOwnCloudPrefs                       ; ENV:/ENVARC:amiowncloud.cfg anlegen
Mount OWNCLOUD:
```

Ohne `ENV:amiowncloud.cfg` startet der Worker trotzdem, aber jede echte
Anfrage scheitert mit einem Netzwerk- bzw. Anmeldefehler.

## Testen

Auf einem Amiga oder im Emulator mit einem echten ownCloud/Nextcloud-Konto:

```
Mount OWNCLOUD:
Dir OWNCLOUD:
MakeDir OWNCLOUD:AmiTest
Copy S:Startup-Sequence OWNCLOUD:AmiTest/
Type OWNCLOUD:AmiTest/Startup-Sequence
Rename OWNCLOUD:AmiTest/Startup-Sequence OWNCLOUD:AmiTest/test.txt
Delete OWNCLOUD:AmiTest ALL
```

`Protect`/`FileNote` sollen ohne Fehler durchlaufen, der Wert bleibt aber
nicht erhalten (siehe oben). Die ausführliche Testreihe (Stufe 1–3) steht
im README des `AMICLOUD:`-Handlers von AmiCloud; dort `AMICLOUD:`/`AmiCloud:`
durch `OWNCLOUD:`/`OwnCloud:` ersetzen.

Fehlersuche:

- `debug=T:owncloud.log` in `ENV:amiowncloud.cfg` schreibt jede HTTP-Anfrage
  des Workers mit (wirkt nach dem nächsten Reboot).
- `Type OWNCLOUD:debuglog` zeigt ein internes Diagnoseprotokoll des
  Handlers (Nachrichten zwischen Handler und Worker).

## Dateien

| Datei | Inhalt |
|---|---|
| `src/handler.c` | Einsprungpunkt, `ACTION_STARTUP`, Packet-Schleife |
| `src/actions.c`, `.h` | eine Funktion je `ACTION_*`-Paket, offene Dateien |
| `src/vol.c`, `.h` | Verzeichnisbaum im RAM als Cache des Servers |
| `src/backend.c`, `.h` | Kommunikation mit dem Worker, Start des Workers |
| `src/proto.h` | Nachrichtenformat Handler ↔ Worker |
| `src/util.c`, `.h` | kleine Hilfsfunktionen ohne libc |
| `src/worker.c` | `amiowncloud-worker` (WebDAV über `webdavcmd.c`) |
| `mount/OWNCLOUD` | DOSDriver (dieselben Werte als Mountlist-Eintrag: `config-examples/OWNCLOUD.mountlist`) |
| `Makefile.amiga-cross` | Cross-Build beider Programme |

`handler.c`, `actions.*`, `vol.*`, `util.*`, `proto.h` und `backend.*` sind
nahezu unveränderte Kopien der Gegenstücke im `AMICLOUD:`-Handler (nur
Geräte-, Volume- und Portnamen unterscheiden sich); der Dateikopf jeder
Datei nennt die Unterschiede.
