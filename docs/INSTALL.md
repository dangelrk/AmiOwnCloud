# AmiOwnCloud installieren

AmiOwnCloud braucht **keinen eigenen Server**: Es spricht per WebDAV mit
einem vorhandenen ownCloud- oder Nextcloud-Server, auf dem man ein Konto
hat. Diese Anleitung beschreibt die Installation auf dem Amiga und am PC.
Bauen aus den Quellen: [BUILD.md](BUILD.md). Bedienung:
[Handbuch](user/AmiOwnCloud-Handbuch.html) bzw. `docs/user/AmiOwnCloud.guide`.

## Amiga

### Voraussetzungen

| Was | Details |
|---|---|
| Betriebssystem | AmigaOS 3.x, Prozessor 68020 oder besser (seit 0.2) |
| TCP/IP-Stack | mit `bsdsocket.library` (Version 4 oder neuer), z. B. Roadshow, AmiTCP/Genesis, Miami. Muss laufen, bevor ein Programm den Server anspricht. |
| AmiSSL 5 | `amisslmaster.library` – von allen Programmen gebraucht, die mit dem Server sprechen (auch bei `http://`) |
| codesets.library | ab Version 6 (Aminet `util/libs/codesets`): Umlaute und andere Sonderzeichen in Dateinamen (seit 0.3); ohne sie gilt ISO-8859-1 |
| MUI | **nicht** nötig – alle Oberflächen sind GadTools/Commodities (intuition, gadtools, commodities.library aus dem Betriebssystem) |
| Stack | nichts einzustellen: die Programme fordern selbst 64 KB an (vbcc `__stack`), `amiowncloud-worker` wird mit 64 KB gestartet, der Handler bekommt 16 KB aus dem DOSDriver |
| Speicher | Der `OWNCLOUD:`-Handler hält eine geöffnete Datei komplett im RAM – für große Dateien entsprechend freien Speicher einplanen |
| Server | Konto auf ownCloud/Nextcloud, am besten mit eigenem App-Passwort |

### Installation mit dem Install-Skript

Die Installationsschublade kommt aus dem Release-Archiv
`AmiOwnCloud-<version>.lha` oder entsteht mit `make dist-amiga`
(`build/dist/AmiOwnCloud/`, als Archiv `build/AmiOwnCloud.lha`). Inhalt:

| Pfad | Inhalt |
|---|---|
| `C/` | alle Programme |
| `Docs/AmiOwnCloud.guide` | Handbuch (AmigaGuide) |
| `Examples/` | Beispielkonfigurationen, `OWNCLOUD.mountlist` |
| `Storage/DOSDrivers/OWNCLOUD` | DOSDriver für den Handler |
| `Install-AmiOwnCloud` | Installationsskript (Commodore-Installer, Icon) |
| `Install-AmiOwnCloud.shell` | dasselbe als Shell-Skript |
| `Uninstall-AmiOwnCloud` | Deinstallationsskript (Icon oder `Execute`) |
| `ReadMe`, `LICENSE`, `THIRD-PARTY-NOTICES` | Kurzbeschreibung, GPLv3, Lizenz der Fremdkomponente Parson (MIT) |

Archiv auspacken (z. B. `LhA x AmiOwnCloud-0.7.lha RAM:`). Alle Teile haben
Workbench-Icons. Zum Installieren in der Schublade das Icon **Install-AmiOwnCloud** doppelt anklicken.
Es startet den Commodore-Installer (bei AmigaOS 3.2 in `SYS:System`, bei
älteren Systemen von der Install-Diskette nach `C:` kopieren); in einer
Shell geht dasselbe mit `Installer Install-AmiOwnCloud`. Ohne Installer gibt es dasselbe als
Shell-Skript:

```
CD RAM:AmiOwnCloud
Execute Install-AmiOwnCloud.shell
```

Der Installer fragt zuerst, welche Teile installiert werden sollen:

1. **OWNCLOUD:-Handler + AmiOwnCloudPrefs** – kopiert `amiowncloud-handler` und
   `amiowncloud-worker` nach `L:`, `AmiOwnCloudPrefs` nach `SYS:Prefs/`, den
   DOSDriver wahlweise nach `DEVS:DOSDrivers/` (Mount bei jedem Start) oder
   `SYS:Storage/DOSDrivers/` (Mount von Hand). Fehlt `amiowncloud.cfg`, bietet
   es an, AmiOwnCloudPrefs zu starten, und danach, `OWNCLOUD:` zu mounten.
2. **AmiOwnCloudSync + AmiOwnCloudSyncPrefs** – `AmiOwnCloudSync` nach
   `SYS:Tools/Commodities/`, `AmiOwnCloudSyncPrefs` nach `SYS:Prefs/`. Bietet an,
   AmiOwnCloudSyncPrefs zu starten und eine Startzeile an `S:User-Startup`
   anzuhängen (Block `;BEGIN AmiOwnCloudSync` … `;END AmiOwnCloudSync`, wird
   nicht doppelt eingetragen).
3. **amiowncloud-sync + amiowncloud-synctest** nach `C:`, auf Wunsch
   `Examples/amiowncloud-sync.cfg.example` als `ENVARC:amiowncloud-sync.cfg`.
4. **Handbuch** nach `HELP:`, sofern `HELP:` zugewiesen ist.

Vorhandene Konfigurationen in `ENV:`/`ENVARC:` und ein vorhandener
DOSDriver werden **nie überschrieben**.

### Aktualisieren

Eine neuere Version installiert man genauso: Archiv auspacken und den
Installer erneut starten. Er vergleicht bei jedem Programm die installierte
Version mit der im Archiv (`$VER`-Angabe, `getversion`):

- **Einsteiger / Geübter Benutzer:** gleiche und ältere Versionen werden
  ersetzt. Ist die installierte Version *neuer*, bleibt sie stehen; der
  Installer nennt sie am Ende.
- **Experte:** für jedes schon installierte Programm zeigt der Installer beide
  Versionen und fragt, ob ersetzt werden soll.

Vorhandene Icons bleiben erhalten (eigene Tooltypes und Positionen gehen
nicht verloren), ebenso Einstellungen und DOSDriver. Die Shell-Variante
`Install-AmiOwnCloud.shell` fragt nur nach, wenn ein installiertes Programm neuer ist
als das im Archiv (Hilfsskript `Install-Datei`, Vergleich mit
`Version … FILE`). Ist `OWNCLOUD:` schon gemountet, arbeitet es bis zum nächsten
Neustart mit der alten Version weiter.

### Manuelle Installation

| Datei aus `C/` bzw. Schublade | Ziel | Hinweis |
|---|---|---|
| `amiowncloud-handler` | `L:` | Pfad steht im DOSDriver (`Handler = L:amiowncloud-handler`) |
| `amiowncloud-worker` | `L:` | fest: der Handler startet `L:amiowncloud-worker` beim Mounten |
| `Storage/DOSDrivers/OWNCLOUD` | `DEVS:DOSDrivers/` oder `SYS:Storage/DOSDrivers/` | in `DEVS:DOSDrivers/` beim Systemstart automatisch bzw. `Mount OWNCLOUD:`; in `SYS:Storage/DOSDrivers/` von Hand: `Mount SYS:Storage/DOSDrivers/OWNCLOUD` |
| `AmiOwnCloudPrefs` | `SYS:Prefs/` | Ort frei wählbar |
| `AmiOwnCloudSync` | `SYS:Tools/Commodities/` | Ort frei wählbar |
| `AmiOwnCloudSyncPrefs` | `SYS:Prefs/` | Ort frei wählbar |
| `amiowncloud-sync`, `amiowncloud-synctest` | `C:` | Ort frei wählbar |
| `Docs/AmiOwnCloud.guide` | `HELP:` | optional |

```
Copy C/amiowncloud-handler L:
Copy C/amiowncloud-worker L:
Copy Storage/DOSDrivers/OWNCLOUD DEVS:DOSDrivers/
Copy C/AmiOwnCloudPrefs SYS:Prefs/
Copy C/AmiOwnCloudSync SYS:Tools/Commodities/
Copy C/AmiOwnCloudSyncPrefs SYS:Prefs/
Copy C/amiowncloud-sync C:
Copy C/amiowncloud-synctest C:
```

Die Programme haben keine Piktogramme (`.info`); aus der Workbench über
„Fenster → Alle Dateien zeigen“ starten oder selbst eines anlegen.

### Konfiguration – wo die Programme suchen

| Programm | liest | schreibt |
|---|---|---|
| `amiowncloud-worker` (OWNCLOUD:) | nur `ENV:amiowncloud.cfg`, nur einmal beim eigenen Start | – |
| AmiOwnCloudPrefs | `ENV:amiowncloud.cfg`, sonst `ENVARC:amiowncloud.cfg` | Benutzen → `ENV:`, Speichern → `ENVARC:` + `ENV:` |
| AmiOwnCloudSync | `ENV:AmiOwnCloudSync.cfg`, sonst `ENVARC:AmiOwnCloudSync.cfg` (beim Start, beim Öffnen des Fensters, vor jedem Autosync, bei „Neu laden“) | – |
| AmiOwnCloudSyncPrefs | `ENV:AmiOwnCloudSync.cfg`, sonst `ENVARC:AmiOwnCloudSync.cfg` | Benutzen → `ENV:`, Speichern → `ENVARC:` + `ENV:` |
| amiowncloud-sync, amiowncloud-synctest | `-c <datei>`, sonst `ENV:amiowncloud-sync.cfg`, sonst `amiowncloud-sync.cfg` im aktuellen Verzeichnis | – |

Beispiele und alle Schlüssel: [config-examples/README.md](../config-examples/README.md).

### OWNCLOUD: mounten

1. `SYS:Prefs/AmiOwnCloudPrefs` starten, Server (z. B.
   `https://cloud.example.com`), Benutzer, Passwort eintragen, **Speichern**.
2. `Mount OWNCLOUD:`, wenn der DOSDriver in `DEVS:DOSDrivers` liegt (dann
   passiert das ab dem nächsten Start auch automatisch), bzw.
   `Mount SYS:Storage/DOSDrivers/OWNCLOUD`, wenn er dort liegt.
3. `Dir OWNCLOUD:`

Der Handler kann nicht ausgehängt werden, und der Worker liest seine
Konfiguration nur beim Start: **Änderungen an `amiowncloud.cfg` wirken erst
nach einem Neustart.**

### AmiOwnCloudSync starten

1. `SYS:Prefs/AmiOwnCloudSyncPrefs` starten, Server, Benutzer, Passwort,
   Intervall und Ordnerpaare eintragen, **Speichern**.
2. `Run >NIL: SYS:Tools/Commodities/AmiOwnCloudSync` (oder per Piktogramm,
   Tooltype `CX_POPKEY=ctrl alt a`).
3. Fenster per Hotkey `ctrl alt a` oder über Exchange.

Für den automatischen Start: Startzeile in `S:User-Startup` (bietet das
Install-Skript an) **nach** dem Start des TCP/IP-Stacks, oder ein
Piktogramm in `SYS:WBStartup`. Startet die Commodity, bevor
`bsdsocket.library`/AmiSSL verfügbar sind, beendet sie sich sofort.

### Deinstallation

```
Execute Uninstall-AmiOwnCloud
```

(aus der Installationsschublade). Löscht Programme, DOSDriver und
`HELP:AmiOwnCloud.guide`, fragt nach dem Löschen der Einstellungen. Von
Hand: AmiOwnCloudSync vorher über Exchange beenden, den Block in
`S:User-Startup` entfernen, `.amicloudsync`-Dateien in den lokalen
Ordnerpaar-Ordnern löschen, neu starten (der Handler lässt sich nicht
aushängen).

## PC (macOS, Linux, Windows)

Am PC gibt es nur die Kommandozeilenprogramme `amiowncloud-sync` und
`amiowncloud-synctest`. Handler, Commodity und Prefs-Programme sind Amiga-only.

```sh
make pc
sudo make install                 # kopiert bin/pc/* nach /usr/local/bin
make install PREFIX=$HOME/.local  # oder anderswohin
make install DESTDIR=/tmp/paket   # für Paketbau
```

Alternativ `bin/pc/amiowncloud-sync` und `bin/pc/amiowncloud-synctest` von Hand an
einen Ort im `PATH` kopieren. Laufzeit-Voraussetzung ist libcurl.

Konfiguration (wenn nicht per `-c <datei>` angegeben):

| System | Datei |
|---|---|
| macOS, Linux (und MSYS2-Umgebung „MSYS“) | `~/.amiowncloud-syncrc` |
| Windows (MinGW, ungetestet) | `amiowncloud-sync.cfg` im aktuellen Verzeichnis |

Vorlage: `config-examples/amiowncloud-sync.cfg.example`. Die Datei enthält das
Passwort im Klartext – z. B. mit `chmod 600 ~/.amiowncloud-syncrc` schützen.

Deinstallation: die beiden Programme aus `$(PREFIX)/bin` und
`~/.amiowncloud-syncrc` löschen.

Getestet ist der PC-Teil bisher nur unter macOS; Linux und Windows sind
vorgesehen, aber ungetestet.
