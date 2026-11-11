# AmiOwnCloud bauen

Diese Anleitung beschreibt, wie man AmiOwnCloud aus den Quellen baut:

- **Amiga** (AmigaOS 3.x, 68020): alle Programme – per Cross-Compiler vbcc
  von macOS, Linux oder Windows aus, oder nativ auf dem Amiga mit dem
  Skript `Build-Amiga`.
- **PC** (macOS, Linux, Windows): die Kommandozeilenprogramme `amiowncloud-sync`
  und `amiowncloud-synctest`.

Installation der fertigen Programme: [INSTALL.md](INSTALL.md).

## Übersicht

| Programm | Verzeichnis | Amiga | PC | Ausgabe |
|---|---|:-:|:-:|---|
| `amiowncloud-handler`, `amiowncloud-worker` | `handler/owncloudhandler` | ✓ | – | `bin/amiga/` |
| `AmiOwnCloudPrefs` | `apps/amiowncloud-prefs` | ✓ | – | `bin/amiga/` |
| `AmiOwnCloudSync` (Commodity) | `apps/amiowncloud-sync-cx` | ✓ | – | `bin/amiga/` |
| `AmiOwnCloudSyncPrefs` | `apps/amiowncloud-sync-prefs` | ✓ | – | `bin/amiga/` |
| `amiowncloud-sync` | `apps/amiowncloud-sync` | ✓ | ✓ | `bin/amiga/`, `bin/pc/` |
| `amiowncloud-synctest` | `apps/amiowncloud-synctest` | ✓ | ✓ | `bin/amiga/`, `bin/pc/` |

AmiOwnCloud hat keine Qt- oder MUI-Programme; alle Amiga-Oberflächen sind
GadTools/Commodities. `make qt` tut in diesem Projekt deshalb nichts.

Build-Ausgaben landen in `bin/pc/`, `bin/amiga/`, `lib/pc/`, `obj/…` und
`build/` (alle von git ignoriert).

## Quellen holen

Die gemeinsamen Bibliotheken (libcloudcore, libcloudhttp, libcloudfs,
libcloudsync, `common/`) und die Top-Level-Buildlogik liegen im Projekt
[AmiCloudCore](https://github.com/dangelrk/AmiCloudCore), das als
Git-Submodul unter `core/` eingebunden ist. Deshalb mit Submodul klonen:

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiOwnCloud.git
cd AmiOwnCloud
```

Wurde ohne `--recurse-submodules` geklont, nachträglich:

```sh
git submodule update --init
```

Fehlt `core/`, bricht `make` mit einem entsprechenden Hinweis ab.

## Top-Level-Ziele

Alle Ziele werden in der Projektwurzel aufgerufen (`make help` zeigt sie):

| Ziel | Wirkung |
|---|---|
| `make pc` | Kommandozeilenprogramme für den Host bauen → `bin/pc/` (Standardziel) |
| `make qt` | Qt6-Programme bauen – in AmiOwnCloud gibt es keine |
| `make amiga` | alle AmigaOS-Programme per vbcc cross-kompilieren → `bin/amiga/` |
| `make dist-amiga` | Amiga-Installationsschublade `build/dist/AmiOwnCloud/` mit Icons erzeugen, dazu `build/AmiOwnCloud.lha` |
| `make release` | wie `dist-amiga`, dazu `build/release/AmiOwnCloud-<version>.lha` und `.readme` |
| `make build-amiga-script` | das native Amiga-Buildskript `Build-Amiga` neu erzeugen |
| `make verify-amiga-script` | … und auf dem Host mit vbcc gegenprüfen |
| `make check-guide` | AmigaGuide-Handbuch in `docs/user/` prüfen (Knoten, Verweise, Latin-1, Zeilenlänge) |
| `make fsuae [PROGS="…"]` | Amiga-Programme im Emulator FS-UAE testen (siehe unten) |
| `make install` | Host-Programme nach `$(PREFIX)/bin` kopieren (`PREFIX=/usr/local`, `DESTDIR` wird unterstützt) |
| `make clean` | alle Build-Ergebnisse entfernen |

Jedes Programm lässt sich auch einzeln bauen, z. B.:

```sh
make -C apps/amiowncloud-sync -f Makefile.pc
make -C apps/amiowncloud-sync-cx -f Makefile.amiga-cross
```

## PC: macOS, Linux, Windows

Voraussetzungen: C99-Compiler, `make`, libcurl (mit Entwickler-Headern) und
`pkg-config`.

| System | Pakete |
|---|---|
| macOS (Homebrew) | `brew install curl pkg-config` |
| Debian/Ubuntu | `sudo apt install build-essential pkg-config libcurl4-openssl-dev` |
| Fedora/RHEL | `sudo dnf install gcc make pkgconf-pkg-config libcurl-devel` |
| Windows | Qt mit MinGW, siehe unten |

Die Makefiles rufen `gcc` auf; unter macOS ist das der Clang der
Xcode-Kommandozeilenwerkzeuge (`xcode-select --install`).

Bauen und optional installieren:

```sh
make pc
sudo make install              # nach /usr/local/bin
# oder z. B.:  make install PREFIX=$HOME/.local
```

Ergebnis: `bin/pc/amiowncloud-sync` und `bin/pc/amiowncloud-synctest`.

**Getestet** wurde der PC-Build unter macOS (Apple Silicon) und Windows 11.
Linux ist in den Makefiles vorgesehen, aber nicht tatsächlich gebaut
worden.

### Windows (nativ mit MinGW aus Qt)

Gebaut unter Windows 11 mit dem MinGW 15.1 (64 Bit), das der Qt Online
Installer mitbringt. Es entstehen native Windows-Programme; MSYS2 wird nicht
gebraucht.

Voraussetzungen:

- **MinGW, CMake, Ninja** über den Qt Online Installer (unter „Build
  Tools“: „MinGW 15.1.0 64-bit“, „CMake“, „Ninja“; Qt selbst wird für
  AmiOwnCloud nicht gebraucht)
- **Git for Windows**: gebaut wird in der Git-Bash (die Makefiles brauchen
  `sh`, `sed` und `date`)
- **libcurl**, einmal aus dem Quelltext (https://curl.se/download/) mit
  demselben MinGW übersetzt: statisch und mit der Windows-eigenen TLS
  (Schannel, Zertifikate aus dem Windows-Zertifikatspeicher). Die Programme
  brauchen dann keine zusätzlichen DLLs. In der Git-Bash, mit dem `PATH`
  wie unten:

  ```sh
  cmake -S curl-8.23.0 -B build-curl -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=C:/winbuild/curl \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=ON \
    -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DCURL_DISABLE_LDAP=ON -DCURL_USE_PKGCONFIG=OFF
  cmake --build build-curl && cmake --install build-curl
  ```

Bauen in der Git-Bash:

```sh
export PATH="/c/Qt/Tools/mingw1510_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
CURL=C:/winbuild/curl
mingw32-make pc PKG_CF="-I$CURL/include -DCURL_STATICLIB" \
  PKG_LF="-L$CURL/lib -lcurl -lnormaliz -lbcrypt -ladvapi32 -lcrypt32 -lsecur32 -lws2_32 -liphlpapi"
```

Ergebnis: `bin/pc/amiowncloud-sync.exe` und `bin/pc/amiowncloud-synctest.exe`,
die nur Windows-eigene DLLs brauchen. Ihre Konfiguration suchen sie unter
Windows als `amiowncloud-sync.cfg` im aktuellen Verzeichnis.

## Amiga: Cross-Compile mit vbcc (von macOS, Linux oder Windows)

### Toolchain

Benötigt wird eine vbcc-Installation mit Target `m68k-amigaos` und einigen
SDKs. Die Umgebungsvariable `VBCC` zeigt auf deren Wurzel; die Makefiles
erwarten folgendes Layout:

| Pfad | Inhalt |
|---|---|
| `$VBCC/bin/` | `vc`, `vbccm68k`, `vasmm68k_mot`, `vlink` |
| `$VBCC/config/aos68k` | vc-Konfiguration für AmigaOS 68k |
| `$VBCC/targets/m68k-amigaos/` | vbcc-Target (`include/`, `lib/`) |
| `$VBCC/sdk/NDK_3.2/Include_H` | AmigaOS NDK 3.2 (C-Header) |
| `$VBCC/sdk/AmiSSL-v5/Developer/include` | AmiSSL-5-SDK |
| `$VBCC/sdk/Roadshow/netinclude` | Roadshow-SDK (bsdsocket-Header) |

(Das MUI-SDK, das die Schwesterprojekte brauchen, ist für AmiOwnCloud
nicht nötig.)

Bezugsquellen:

- vbcc, vasm, vlink (Quellen und Target `m68k-amigaos`):
  <http://sun.hasenbraten.de/vbcc/>
- NDK 3.2: von Hyperion Entertainment
- AmiSSL-SDK: aus den AmiSSL-Releases,
  <https://github.com/jens-maus/amissl/releases>
- Roadshow-SDK: Aminet

### Bauen

```sh
export VBCC=/pfad/zu/vbcc
make amiga
```

`make amiga` stellt `$VBCC/bin` selbst an den Anfang von `PATH`. Ziel ist
ein 68020 mit AmigaOS 3.x (seit 0.2); Fließkomma-Mathematik kommt aus `-lmieee`.
Ergebnis: alle Programme in `bin/amiga/`.

Danach die Installationsschublade erzeugen:

```sh
make dist-amiga
```

Sie liegt in `build/dist/AmiOwnCloud/` (und als `build/AmiOwnCloud.lha`)
und enthält:

| Pfad in der Schublade | Inhalt |
|---|---|
| `C/` | alle Programme aus `bin/amiga/` |
| `Docs/` | `AmiOwnCloud.guide` |
| `Examples/` | Inhalt von `config-examples/` |
| `Storage/DOSDrivers/OWNCLOUD` | DOSDriver für den Handler (über `DIST_EXTRA` im `Makefile`) |
| `Install-AmiOwnCloud` (Installer), `Install-AmiOwnCloud.shell`, `Uninstall-AmiOwnCloud`, `ReadMe` | aus `install/amiga/` |
| `LICENSE` | GPLv3 |
| `*.info` | erzeugt nach `install/amiga/icons.spec` |

Zu allen Teilen der Schublade erzeugt `core/tools/mkicon.py` Workbench-Icons
nach `install/amiga/icons.spec` (Bild, Standardprogramm, Tooltypes, Position).
Gepackt wird mit `core/tools/lhapack.py` zu `build/AmiOwnCloud.lha` – einem eigenen
LHA-Packer (Methode `-lh5-`, Level-2-Köpfe) in reinem Python, weil es unter
macOS keinen LHA-*Packer* gibt (Homebrews `lha` ist Lhasa und packt nur aus).
Die Archive lassen sich mit Lhasa und mit dem `lha` von AmigaOS 3.2 auspacken.

### Release

```sh
make release
```

baut `make dist-amiga` und legt in `build/release/` die Dateien für
Aminet/GitHub ab: `AmiOwnCloud-<version>.lha` und `AmiOwnCloud-<version>.readme` (aus
`install/amiga/aminet.readme`, `@VERSION@` wird ersetzt). Die Version steht nur in
`core/libcloudcore/include/cloudcore/version.h` (`AC_VERSION`), das Datum
ist immer der Tag des Bauens (`make` übergibt `-DAC_BUILD_DATE=TT.MM.JJJJ`,
`make release` baut alle Amiga-Programme neu). Daraus bauen alle Programme
ihre `$VER`-Angabe, und `make dist-amiga` setzt beide in die Installer-Skripte ein (`@VERSION@`,
`@DATE@`, für die Shell-Variante `@VER_MAJOR@`/`@VER_REVN@`). Weil die
Installer nur ersetzen, wenn die Version im Archiv nicht älter ist, **vor
jedem Release `AC_VERSION` erhöhen**. Ist Lhasa
installiert, prüft `lha t` das Archiv gleich mit.


### Cross-Compile unter Windows (MSYS2) – ungetestet

Der Amiga-Build ist unter Windows nicht getestet; vorgesehen ist dieser
Weg:

1. [MSYS2](https://www.msys2.org/) installieren und in der Umgebung
   „MSYS“ die Werkzeuge holen: `pacman -S make git python`
   (`python` wird nur für `make build-amiga-script`,
   `make verify-amiga-script` und `make check-guide` gebraucht).
2. vbcc, vasm und vlink als Windows-Programme bereitstellen – aus den
   Quellen von http://sun.hasenbraten.de/vbcc/ selbst übersetzen (z. B.
   mit `gcc` aus MSYS2) oder vorkompiliert beziehen – und mit Target und
   SDKs im oben beschriebenen Layout ablegen, am besten in einem Pfad
   ohne Leerzeichen, z. B. `C:\vbcc`.
3. In der MSYS2-Shell den Pfad in MSYS2-Schreibweise angeben und bauen:

   ```sh
   export VBCC=/c/vbcc
   make amiga
   make dist-amiga
   ```

Auch hier gilt die folgende Stolperfalle: Enthält `config/aos68k`
absolute Pfade, müssen sie zum Windows-Pfad passen (sonst
`VC_CONFIG=…`).

### Stolperfalle: absolute Pfade in `config/aos68k`

Die vc-Konfigurationsdatei `$VBCC/config/aos68k` enthält bei vielen
Installationen **absolute Pfade** (z. B. auf `startup.o` und die
Bibliotheken). Wurde die Toolchain nach der Installation verschoben,
laufen alle Compile-Schritte durch und erst der Link-Schritt scheitert mit
„Cannot open …/startup.o“. Abhilfe: die Datei anpassen oder eine
korrigierte Kopie angeben:

```sh
make amiga VC_CONFIG=/pfad/zu/meiner/aos68k
make dist-amiga VC_CONFIG=/pfad/zu/meiner/aos68k
```

## Amiga: nativ bauen mit `Build-Amiga`

Auf einem Amiga (oder im Emulator) mit installiertem vbcc lässt sich ohne
`make` bauen. Voraussetzungen:

- vbcc mit Target m68k-amigaos, `vc` im Suchpfad, NDK-Includes über die
  vbcc-Konfiguration erreichbar
- AmiSSL-5-SDK und Roadshow-SDK (netinclude)
- die Projektquellen einschließlich `core/`

Die SDK-Pfade stehen oben im Skript als Variablen:

```
Set AMISSL_INC "AmiSSL:Developer/include"
Set NET_INC    "NetInclude:"
```

Entweder dort anpassen oder die Assigns passend setzen, z. B.:

```
Assign AmiSSL: Work:SDK/AmiSSL
Assign NetInclude: Work:SDK/Roadshow/netinclude
```

Dann im Projektverzeichnis:

```
Execute Build-Amiga
```

Ergebnis: `bin/amiga/<Programm>`, Objektdateien in
`obj/amiga/<Programm>/`.

`Build-Amiga` ist eine **erzeugte Datei** – nicht von Hand ändern. Sie
entsteht aus den `Makefile.amiga-cross`-Dateien:

```sh
make build-amiga-script          # neu erzeugen
make verify-amiga-script         # auf dem Host nach sh übersetzen und mit vbcc ausführen
```

`make verify-amiga-script` baut dabei alle Programme vollständig bis zum
Link. **Auf echter Amiga-Hardware ist `Build-Amiga` selbst bisher
ungetestet.**

## Testen

### Host-Regressionstest für die Ordnerpaare

`scripts/test_pairs.sh` übersetzt `apps/amiowncloud-sync-cx/tests/test_pairs.c`
zusammen mit den echten Quellen `apps/amiowncloud-sync-cx/src/pairs.c` und
`core/libcloudcore/src/config.c` mit dem Host-Compiler (`cc`) und prüft,
dass Ordnerpaare – auch mit sehr langen Pfaden – verlustfrei gespeichert
und wieder geladen werden. Kein Amiga und kein vbcc nötig:

```sh
scripts/test_pairs.sh
```

### Testsuite gegen einen echten Server

`amiowncloud-synctest` (PC und Amiga) prüft den Zwei-Wege-Abgleich gegen einen
echten ownCloud/Nextcloud-Server – siehe Handbuch
[AmiOwnCloud-Handbuch.html](user/AmiOwnCloud-Handbuch.html), Kapitel
„amiowncloud-synctest“.

### Im Emulator (FS-UAE)

`make fsuae` (Skript `core/tools/run-fsuae.sh`) kopiert die gebauten Amiga-Programme in einen
Host-Ordner, der in FS-UAE als Laufwerk eingehängt ist, und startet
FS-UAE:

```sh
export FSUAE_CONFIG=/pfad/zu/entwicklung.fs-uae   # Pflicht
export FSUAE_SHARED=/pfad/zu/shared               # Standard: build/fsuae-shared
export FSUAE_BIN=/pfad/zu/fs-uae                  # Standard: fs-uae aus PATH bzw. FS-UAE.app
make fsuae                                        # alle Programme
make fsuae PROGS="amiowncloud-handler amiowncloud-worker"   # nur diese
```

In der FS-UAE-Konfiguration muss der geteilte Ordner als Laufwerk
eingetragen sein, z. B.:

```
hard_drive_1 = /pfad/zu/shared
hard_drive_1_label = SHARED
```

Im Amiga-Shell liegen die Programme dann unter `SHARED:`.
