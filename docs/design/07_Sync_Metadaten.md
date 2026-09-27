# Synchronisation – Metadaten

## Ziel

Der Synchronisationsalgorithmus soll zuverlässig erkennen können:

- neue Dateien
- geänderte Dateien
- gelöschte Dateien
- Konflikte

Dabei soll niemals eine Datei unbemerkt überschrieben werden.

---

# Lokale Sync-Datenbank

Jeder Client besitzt eine lokale Datenbank: JSON, Datei `.amicloudsync` im
Sync-Wurzelverzeichnis (implementiert über die vorhandene parson-Bibliothek,
libcloudsync/syncstate.{c,h}).

Diese beschreibt den Zustand des letzten erfolgreichen Synchronisationslaufs.

Für jede Datei werden gespeichert:

- relativer Pfad
- Dateityp
- Dateigröße
- lokaler mtime
- MD5
- Server-Revision
- Server-Datei-ID (optional)
- letzter erfolgreicher Sync-Zeitpunkt (optional)

Diese Datenbank wird **erst nach vollständig erfolgreichem Sync** aktualisiert.
Schlägt eine einzelne Aktion fehl, behält nur ihr Pfad den alten Eintrag —
der nächste Lauf wiederholt genau diesen Pfad, ohne den Rest neu anzufassen.

---

# Serverdaten

Der Server speichert für jede Datei:

- Datei-ID
- Besitzer (owner_id)
- Pfad
- Dateityp
- Größe
- MD5
- Client-mtime
- Upload-Zeit
- Revision
- globale Server-Revision

Zusätzlich (Soft-Delete, implementiert):

- Löschstatus (Tombstone: Zeile bleibt in der DB, `deleted_at` gesetzt)
- Löschzeit

Die Dateibytes selbst werden beim Löschen sofort und endgültig entfernt —
nur die Metadaten-Zeile bleibt als Tombstone erhalten. Versionierung
mehrerer Inhalte derselben Datei ist kein Bestandteil dieses Konzepts,
sondern Aufgabe des separaten VCS (siehe 08_VCS_Grundkonzept.md).

---

# Änderungszeit

Die mtime stammt grundsätzlich vom Client.

Beim Upload wird die lokale Änderungszeit übernommen.

Sie dient ausschließlich als Information und Optimierung.

Sie entscheidet niemals allein über Änderungen.

Die eigentliche Wahrheit ist der Dateiinhalt (MD5).

---

# MD5

MD5 dient ausschließlich als Inhaltsfingerabdruck.

Der MD5 entscheidet, ob sich der Dateiinhalt geändert hat.

MD5 soll lokal gecacht werden.

---

# Revision

Jede Datei besitzt eine Revision.

Neue Datei:

Revision = 1

Bei jeder Inhaltsänderung:

Revision++

Umbenennen oder Verschieben erhöht die Revision nicht.

---

# Globale Server-Revision

Zusätzlich besitzt jeder Benutzer eine eigene globale Revision
(`users.global_revision` — pro Benutzer, nicht serverweit über alle
Benutzer hinweg, siehe ADR 04 zu Multiuser).

Jede Änderung an irgendeiner Datei dieses Benutzers erhöht diese.

Beispiel:

ServerRevision = 138

Datei A Revision = 135
Datei B Revision = 138

Der Client merkt sich:

"Letzter erfolgreicher Sync bis ServerRevision 138"

Ist die ServerRevision unverändert, kann der Client zukünftige Optimierungen durchführen:
den vollständigen Verzeichnis-Scan der Gegenseite überspringen und stattdessen
die eigene Baseline als Stellvertreter für den (nachweislich unveränderten)
Remote-Stand verwenden.

Für einen echten Änderungsabgleich (statt nur "hat sich überhaupt etwas
geändert") liefert der Server zusätzlich einen `changes`-Endpunkt: alle
Zeilen mit `change_seq` größer als ein übergebener Wert, einschließlich
Tombstones. Das ist die eigentliche Delta-Quelle für einen Client, der
nicht bei jedem Sync den kompletten Baum neu auflisten will.

---

# Konflikterkennung

Der Last Sync State ist die einzige Referenz.

Verglichen werden:

- Local
- Remote
- Last Sync

Beide geändert, unterschiedlicher Inhalt:

→ Konflikt

Beide geändert, aber auf denselben Inhalt konvergiert:

→ kein Transfer, Baseline wird trotzdem aktualisiert

Nur lokal geändert:

→ Upload

Nur remote geändert:

→ Download

Ein Konflikt wird niemals automatisch aufgelöst — insbesondere nicht anhand
der mtime. Er bleibt bei jedem weiteren Sync-Lauf bestehen (der alte
Baseline-Eintrag bleibt unverändert erhalten), bis der Benutzer ihn manuell
auflöst.

---

# Löschungen

Eine Datei fehlt auf genau einer lebenden Seite (lokal oder remote), existiert
aber im Last Sync State:

- Entspricht ihr aktueller Stand auf der verbliebenen Seite noch genau dem
  Last Sync State (Inhalt unverändert seit dem letzten Sync) → die Datei wurde
  auf der anderen Seite gelöscht → Löschung wird propagiert.
- Weicht der Stand auf der verbliebenen Seite vom Last Sync State ab (sie
  wurde dort zusätzlich geändert) → Konflikt, keine automatische Löschung.

Fehlt eine Datei auf **beiden** lebenden Seiten, aber noch im Last Sync State,
wird ihr Baseline-Eintrag stillschweigend entfernt — nichts zu tun.

Die Löschungs-Propagation selbst arbeitet Pfad für Pfad, nicht als
rekursiver Baum-Löschvorgang: ein Verzeichnis wird erst entfernt, wenn
sämtlicher Inhalt darunter bereits weg ist. Damit kann eine einzelne Datei
innerhalb eines sonst gelöschten Verzeichnisses ihren eigenen, unabhängigen
Konflikt behalten, statt von einer pauschalen Rekursion mitgerissen zu werden.

---

# Architektur

Der Server verwaltet:

- Dateien
- Revisionen
- Benutzer

Der Client verwaltet:

- Synchronisationslogik
- Konflikte
- Last Sync State

Der Server trifft niemals Synchronisationsentscheidungen.