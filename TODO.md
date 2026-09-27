# ToDo

## Zeitzone wird von `AmiCloudSync` und `webdavsync` ignoriert (Amiga)

**Status:** offen, bekannt seit 27.09.2026

Die Amiga-Systemuhr kennt keine Zeitzone. `libcloudfs` rechnet lokale
Datumsangaben deshalb mit `gmt_offset`/`dst` in UTC um – aber nur,
wenn das Programm die Werte vorher mit `localfs_set_tz()` setzt
(`core/libcloudfs/src/localfs_amiga.c`). Weder `AmiCloudSync`
(`apps/amicloud-sync-cx`) noch `webdavsync` tun das; ihre
Konfigurationen (`AmiCloudSync.cfg`, `webdavsync.cfg`) bieten die
Schlüssel bisher auch nicht an.

Nicht betroffen ist der `OWNCLOUD:`-Handler: Sein Worker rechnet selbst
mit `gmt_offset`/`dst` aus `owncloud.cfg`.

**Auswirkung:** Auf dem Amiga werden lokale Dateizeiten so behandelt,
als stünde die Uhr auf UTC. Beim Vergleich mit den Zeiten des
WebDAV-Servers können Dateien dadurch als geändert gelten, obwohl sie
es nicht sind, oder Konflikte falsch entschieden werden. Auf dem PC
gibt es das Problem nicht.

**Lösung (Vorschlag):** `gmt_offset`/`dst` in beiden Konfigurationen
unterstützen (für `AmiCloudSync` auch in `AmiCloudSyncPrefs`) und nach
dem Laden `localfs_set_tz(cfg.gmt_offset, cfg.dst);` aufrufen, wie es
`amicloud-shell` im Projekt AmiCloud tut. Danach auf dem Amiga prüfen:
Ein zweiter Abgleich direkt nach dem ersten darf nichts mehr übertragen.
