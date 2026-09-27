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
| Betriebssystem | AmigaOS 3.x, Prozessor 68000 oder besser |
| TCP/IP-Stack | mit `bsdsocket.library` (Version 4 oder neuer), z. B. Roadshow, AmiTCP/Genesis, Miami. Muss laufen, bevor ein Programm den Server anspricht. |
| AmiSSL 5 | `amisslmaster.library` – von allen Programmen gebraucht, die mit dem Server sprechen (auch bei `http://`) |
| MUI | **nicht** nötig – alle Oberflächen sind GadTools/Commodities (intuition, gadtools, commodities.library aus dem Betriebssystem) |
| Stack | nichts einzustellen: die Programme fordern selbst 64 KB an (vbcc `__stack`), `owncloud-worker` wird mit 64 KB gestartet, der Handler bekommt 16 KB aus dem DOSDriver |
| Speicher | Der `OWNCLOUD:`-Handler hält eine geöffnete Datei komplett im RAM – für große Dateien entsprechend freien Speicher einplanen |
| Server | Konto auf ownCloud/Nextcloud, am besten mit eigenem App-Passwort |

### Installation mit dem Install-Skript

Die Installationsschublade entsteht mit `make dist-amiga`
(`build/dist/AmiOwnCloud/`, als Archiv `build/AmiOwnCloud.lha`). Inhalt:

| Pfad | Inhalt |
|---|---|
| `C/` | alle Programme |
| `Docs/AmiOwnCloud.guide` | Handbuch (AmigaGuide) |
| `Examples/` | Beispielkonfigurationen, `OWNCLOUD.mountlist` |
| `Storage/DOSDrivers/OWNCLOUD` | DOSDriver für den Handler |
| `Install-AmiOwnCloud`, `Uninstall-AmiOwnCloud` | Installations-/Deinstallationsskript |
| `ReadMe`, `LICENSE`, `THIRD-PARTY-NOTICES` | Kurzbeschreibung, GPLv3, Lizenz der Fremdkomponente Parson (MIT) |

Auf dem Amiga in einer Shell:

```
LhA x AmiOwnCloud.lha RAM:
CD RAM:AmiOwnCloud
Execute Install-AmiOwnCloud
```

Das Skript fragt jeden Teil einzeln ab (`y`/`n`):

1. **OWNCLOUD:-Handler + OwnCloudPrefs** – kopiert `owncloud-handler` und
   `owncloud-worker` nach `L:`, `OwnCloudPrefs` nach `SYS:Prefs/`, den
   DOSDriver wahlweise nach `DEVS:DOSDrivers/` (Mount bei jedem Start) oder
   `SYS:Storage/DOSDrivers/` (Mount von Hand). Fehlt `owncloud.cfg`, bietet
   es an, OwnCloudPrefs zu starten, und danach `Mount OWNCLOUD:`.
2. **AmiCloudSync + AmiCloudSyncPrefs** – `AmiCloudSync` nach
   `SYS:Tools/Commodities/`, `AmiCloudSyncPrefs` nach `SYS:Prefs/`. Bietet an,
   AmiCloudSyncPrefs zu starten und eine Startzeile an `S:User-Startup`
   anzuhängen (Block `;BEGIN AmiCloudSync` … `;END AmiCloudSync`, wird
   nicht doppelt eingetragen).
3. **webdavsync + webdavsynctest** nach `C:`, auf Wunsch
   `Examples/webdavsync.cfg.example` als `ENVARC:webdavsync.cfg`.
4. **Handbuch** nach `HELP:`, sofern `HELP:` zugewiesen ist.

Vorhandene Konfigurationen in `ENV:`/`ENVARC:` und ein vorhandener
DOSDriver werden **nie überschrieben**.

### Manuelle Installation

| Datei aus `C/` bzw. Schublade | Ziel | Hinweis |
|---|---|---|
| `owncloud-handler` | `L:` | Pfad steht im DOSDriver (`Handler = L:owncloud-handler`) |
| `owncloud-worker` | `L:` | fest: der Handler startet `L:owncloud-worker` beim Mounten |
| `Storage/DOSDrivers/OWNCLOUD` | `DEVS:DOSDrivers/` oder `SYS:Storage/DOSDrivers/` | `Mount OWNCLOUD:` sucht in beiden |
| `OwnCloudPrefs` | `SYS:Prefs/` | Ort frei wählbar |
| `AmiCloudSync` | `SYS:Tools/Commodities/` | Ort frei wählbar |
| `AmiCloudSyncPrefs` | `SYS:Prefs/` | Ort frei wählbar |
| `webdavsync`, `webdavsynctest` | `C:` | Ort frei wählbar |
| `Docs/AmiOwnCloud.guide` | `HELP:` | optional |

```
Copy C/owncloud-handler L:
Copy C/owncloud-worker L:
Copy Storage/DOSDrivers/OWNCLOUD DEVS:DOSDrivers/
Copy C/OwnCloudPrefs SYS:Prefs/
Copy C/AmiCloudSync SYS:Tools/Commodities/
Copy C/AmiCloudSyncPrefs SYS:Prefs/
Copy C/webdavsync C:
Copy C/webdavsynctest C:
```

Die Programme haben keine Piktogramme (`.info`); aus der Workbench über
„Fenster → Alle Dateien zeigen“ starten oder selbst eines anlegen.

### Konfiguration – wo die Programme suchen

| Programm | liest | schreibt |
|---|---|---|
| `owncloud-worker` (OWNCLOUD:) | nur `ENV:owncloud.cfg`, nur einmal beim eigenen Start | – |
| OwnCloudPrefs | `ENV:owncloud.cfg`, sonst `ENVARC:owncloud.cfg` | Benutzen → `ENV:`, Speichern → `ENVARC:` + `ENV:` |
| AmiCloudSync | `ENV:AmiCloudSync.cfg`, sonst `ENVARC:AmiCloudSync.cfg` (beim Start, beim Öffnen des Fensters, vor jedem Autosync, bei „Neu laden“) | – |
| AmiCloudSyncPrefs | `ENV:AmiCloudSync.cfg`, sonst `ENVARC:AmiCloudSync.cfg` | Benutzen → `ENV:`, Speichern → `ENVARC:` + `ENV:` |
| webdavsync, webdavsynctest | `-c <datei>`, sonst `ENV:webdavsync.cfg`, sonst `webdavsync.cfg` im aktuellen Verzeichnis | – |

Beispiele und alle Schlüssel: [config-examples/README.md](../config-examples/README.md).

### OWNCLOUD: mounten

1. `SYS:Prefs/OwnCloudPrefs` starten, Server (z. B.
   `https://cloud.example.com`), Benutzer, Passwort eintragen, **Speichern**.
2. `Mount OWNCLOUD:` (liegt der DOSDriver in `DEVS:DOSDrivers`, passiert das
   beim nächsten Start automatisch).
3. `Dir OWNCLOUD:`

Der Handler kann nicht ausgehängt werden, und der Worker liest seine
Konfiguration nur beim Start: **Änderungen an `owncloud.cfg` wirken erst
nach einem Neustart.**

### AmiCloudSync starten

1. `SYS:Prefs/AmiCloudSyncPrefs` starten, Server, Benutzer, Passwort,
   Intervall und Ordnerpaare eintragen, **Speichern**.
2. `Run >NIL: SYS:Tools/Commodities/AmiCloudSync` (oder per Piktogramm,
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
Hand: AmiCloudSync vorher über Exchange beenden, den Block in
`S:User-Startup` entfernen, `.amicloudsync`-Dateien in den lokalen
Ordnerpaar-Ordnern löschen, neu starten (der Handler lässt sich nicht
aushängen).

## PC (macOS, Linux, Windows)

Am PC gibt es nur die Kommandozeilenprogramme `webdavsync` und
`webdavsynctest`. Handler, Commodity und Prefs-Programme sind Amiga-only.

```sh
make pc
sudo make install                 # kopiert bin/pc/* nach /usr/local/bin
make install PREFIX=$HOME/.local  # oder anderswohin
make install DESTDIR=/tmp/paket   # für Paketbau
```

Alternativ `bin/pc/webdavsync` und `bin/pc/webdavsynctest` von Hand an
einen Ort im `PATH` kopieren. Laufzeit-Voraussetzung ist libcurl.

Konfiguration (wenn nicht per `-c <datei>` angegeben):

| System | Datei |
|---|---|
| macOS, Linux (und MSYS2-Umgebung „MSYS“) | `~/.webdavsyncrc` |
| Windows (MinGW, ungetestet) | `webdavsync.cfg` im aktuellen Verzeichnis |

Vorlage: `config-examples/webdavsync.cfg.example`. Die Datei enthält das
Passwort im Klartext – z. B. mit `chmod 600 ~/.webdavsyncrc` schützen.

Deinstallation: die beiden Programme aus `$(PREFIX)/bin` und
`~/.webdavsyncrc` löschen.

Getestet ist der PC-Teil bisher nur unter macOS; Linux und Windows sind
vorgesehen, aber ungetestet.
