# Änderungen – Rührlink Firmware (Box)

Neueste Version oben. Für Nutzer geschrieben; Technisches nur, wo es für Updates wichtig ist.
Einträge kommen mit jeder Änderung unter **Unveröffentlicht** und wandern beim Versionssprung in die neue Version.
Aus diesem Changelog entstehen die Notizen der GitHub-Releases (siehe [docs/updates.md](docs/updates.md)).
Sicherheitsrelevante Änderungen sind mit **Sicherheit** gekennzeichnet. Nummern wie F17 verweisen auf
`user_feedback/fehlerliste.md`. Die App hat ein eigenes Changelog (`ruehrlink_app/CHANGELOG.md`).

## Unveröffentlicht

### Sicherheit
- **Alles löschen:** Zurücksetzen aus der App kann jetzt auch alle Rezepte, Fotos, Programme und Protokolle
  löschen (microSD wird formatiert, interner Speicher gelöscht) – vor Weitergabe oder Verkauf. Das 5×-Aus/Ein
  lässt die Daten wie bisher stehen.
- Kopplung: Eine laufende Kopplungsanfrage kann nicht mehr von einem anderen Gerät im WLAN ersetzt und damit
  übernommen werden. Weitere Anfragen bekommen bis zu 60 s „bitte später“.
- Die App-Schnittstelle ist nur noch im Heim-WLAN erreichbar, nicht über das WLAN der Maschine.
- Zugangs-Token werden mit fester Laufzeit verglichen.
- Meldeweg für Sicherheitslücken: `SECURITY.md`.

## 0.6.0 – 24.11.2026

**Nur noch für die Rührlink-Platine (ESP32-C6).** Frühe Entwicklungsboxen mit ESP32-S3 werden nicht mehr
unterstützt; ein Update-Image für den falschen Chip lehnt die Box gleich zu Beginn ab.

### Neu
- Firmware für die Rührlink-Platine: 8 MB Flash, microSD per SPI, Logs und Flashen über USB-C.
- Status-LED rot/grün: Signale nur noch kurz; einzige Daueranzeige ist ein schwaches Grün, solange die Maschine
  verbunden ist.
- Werksdaten: Einrichtungs-Code und Seriennummer werden bei der Herstellung gespeichert und überstehen Werksreset
  und Updates; das Etikett bleibt gültig.
- `/api/v1/info` meldet den Chip (`chip`).
- Vorbereitet: Treiber für eine WS2812-Schüsselbeleuchtung an der Buchse der Platine (noch nicht in der App).

### Behoben
- Maschine koppeln scheiterte beim Übergeben der Zugangsdaten, und die Waage verband sich nach dem ersten Mal
  nicht mehr (Zertifikat ging nach der ersten Verbindung verloren, F28).

## 0.5.1 – 22.11.2026

### Behoben
- Der Start am Gerät wird zuverlässig erkannt, auch wenn Statusmeldungen verloren gehen; der Programmstand wird
  während eines Programms regelmäßig neu verteilt (F26).

### Geändert
- Ausführlicheres Protokoll der Programm-Zustandswechsel mit Anlass und freiem Speicher.

## 0.5.0 – 22.11.2026

### Neu
- `GET /api/v1/diag` mit Zustand und den letzten Log-Ausgaben des Rührlink für „Problem melden“ (Fähigkeit
  `diag`). **Sicherheit:** Passwörter, Einrichtungs-Code und Tokens werden vor dem Speichern maskiert.
- „Maschine neu koppeln“ ohne Werksreset: `POST /api/v1/machine/repair` (Fähigkeit `machine.repair`).
  Heim-WLAN, Handys und Rezepte bleiben.
- Rezepte und Fotos liegen auf einer microSD (SPI). Ohne Karte wie bisher im internen Speicher; vorhandene Daten
  werden einmal auf eine leere Karte kopiert. `/info` meldet `storage.backend` = `sd` oder `flash`.

### Geändert
- Der Name des Maschinen-WLANs wird einmal festgeschrieben und ändert sich durch Updates nicht mehr (F18).
- `appliance.paired` in `/info` wird dauerhaft gemerkt.
- Das eigene Maschinen-WLAN erscheint nicht mehr in der WLAN-Liste der Einrichtung.

## 0.4.0 – 21.11.2026

### Neu
- Der Rührlink meldet der App, welche Funktionen er kann. App und Rührlink lassen sich dadurch unabhängig
  voneinander aktualisieren.
- Updates gibt es als eine einzige Datei `ruehrlink-<version>.rlfw` auf der GitHub-Release-Seite; die App
  spielt sie auf.

## 0.3.3 – 15.11.2026

### Geändert
- Waage verbindet sich etwa doppelt so schnell (gut 2,5 s statt 5 s).
- Ausgeschaltete Maschine wird schneller erkannt: spätestens nach etwa 8 s statt bis zu 20 s; bei offener Waage
  meist nach wenigen Sekunden.
- Waage liefert bis zu 20 Werte pro Sekunde an die App (vorher 10).

## 0.3.2 – 15.11.2026

### Neu
- Rezepte mit Zutaten und Rührelement pro Schritt werden angenommen.

## 0.3.1 – 15.11.2026

### Neu
- Waage der Maschine: Werte gehen live an die App, Tara rechnet die Box.
- Rezepte und Fotos werden auf der Box gespeichert, alle gekoppelten Handys sehen dieselben.
- Programme: Nach dem Abkühlen unter 60 °C wird die gewünschte schnellere Stufe erneut gesendet (B1).

### Behoben
- Bluetooth wird nach der Einrichtung wirklich abgeschaltet; damit auch kein Absturz mehr nach 10 min (F3, F17).
- Nach einem Stromausfall während des Speicherns bleiben keine halb geschriebenen Dateien zurück.

## 0.3.0 – 15.11.2026

### Geändert
- Das Produkt heißt **Rührlink** (vorher Freikoch).

## 0.2.0 – 15.11.2026

### Neu
- Firmware-Update über die App: nur signierte Firmware, Prüfsumme, Probebetrieb nach dem Neustart, bei Problemen
  automatisch zurück zur alten Version. Nie während ein Programm läuft. **Sicherheit**
- Warmhalten der Maschine wird gemeldet (F4).

## 0.1.0 – 12.–13.11.2026

### Neu
- Erste Version: Box spielt für die Kenwood Cooking Chef XL (KCL95) die abgeschaltete Cloud, strikt lokal im
  Heim-WLAN.
- Maschine koppeln (mit Zertifikats-Pinning), Status, Temperatur, Zeit und Stufe senden; Start am Drehknopf.
- Programme (feste Abläufe) laufen auf der Box.
- Einrichtung per Bluetooth, weitere Handys koppeln per Bestätigung, Werksreset, Status-LED.

### Behoben
- Ausgeschaltete Maschine wird in Sekunden statt nach einer Minute erkannt (F7).
- Kopf auf/zu und Ruhephasen brechen Programme nicht mehr ab (F9–F12).
- Hinweis „Schüssel einsetzen“ leuchtete dauerhaft (F1, F2).
