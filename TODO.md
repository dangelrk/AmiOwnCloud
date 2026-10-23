# ToDo

## Zeitzone wird von `AmiOwnCloudSync` und `amiowncloud-sync` ignoriert (Amiga)

**Status:** offen, bekannt seit 27.09.2026

Die Amiga-Systemuhr kennt keine Zeitzone. `libcloudfs` rechnet lokale
Datumsangaben deshalb mit `gmt_offset`/`dst` in UTC um – aber nur,
wenn das Programm die Werte vorher mit `localfs_set_tz()` setzt
(`core/libcloudfs/src/localfs_amiga.c`). Weder `AmiOwnCloudSync`
(`apps/amiowncloud-sync-cx`) noch `amiowncloud-sync` tun das; ihre
Konfigurationen (`AmiOwnCloudSync.cfg`, `amiowncloud-sync.cfg`) bieten die
Schlüssel bisher auch nicht an.

Nicht betroffen ist der `OWNCLOUD:`-Handler: Sein Worker rechnet selbst
mit `gmt_offset`/`dst` aus `amiowncloud.cfg`.

**Auswirkung:** Auf dem Amiga werden lokale Dateizeiten so behandelt,
als stünde die Uhr auf UTC. Beim Vergleich mit den Zeiten des
WebDAV-Servers können Dateien dadurch als geändert gelten, obwohl sie
es nicht sind, oder Konflikte falsch entschieden werden. Auf dem PC
gibt es das Problem nicht.

**Lösung (Vorschlag):** `gmt_offset`/`dst` in beiden Konfigurationen
unterstützen (für `AmiOwnCloudSync` auch in `AmiOwnCloudSyncPrefs`) und nach
dem Laden `localfs_set_tz(cfg.gmt_offset, cfg.dst);` aufrufen, wie es
`amicloud-shell` im Projekt AmiCloud tut. Danach auf dem Amiga prüfen:
Ein zweiter Abgleich direkt nach dem ersten darf nichts mehr übertragen.
