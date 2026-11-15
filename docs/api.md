# Lokale App-API des Rührlink (v1, Entwurf)

Die API ist nur im Heim-WLAN erreichbar. Sie ist die einzige Schnittstelle zwischen
Brücke und App und wird in beiden Repos (`ruehrlink_esp`, `ruehrlink_app`) gegen diese Datei
entwickelt. Änderungen hier zuerst, dann im Code.

## Finden

- mDNS-Dienst `_ruehrlink._tcp`, Port 80
- TXT-Records: `id=<geräte-id>`, `fw=<version>`, `api=1`, `name=<anzeigename>`

## Koppeln & Anmelden

Die Brücke hat **keine Taste für Kunden** (Entscheidung 12.11.). Gekoppelt wird so:

1. **Erstes Handy:** Bluetooth-Einrichtung. Der QR-Code auf dem Etikett enthält den Einrichtungs-Code;
   die App erhält den Token direkt im Provisioning-Ablauf.
2. **Weitere Handys**, zwei Wege:
   - a) ebenfalls per Bluetooth mit dem QR-Code vom Etikett (Heim-WLAN ist dann schon eingerichtet), oder
   - b) **Bestätigung durch ein schon gekoppeltes Handy:**
     1. Neue App: `POST /api/v1/pair` mit `{"name":"Pixel 8a"}` → `202 {"pairing":"waiting","id":"<anfrage-id>"}`,
        die LED der Brücke blinkt blau.
     2. Alle gekoppelten Apps erhalten per WS `{"t":"pairRequest","d":{"id":"…","name":"Pixel 8a"}}` und fragen
        „Neues Gerät möchte sich verbinden – erlauben?“.
     3. Eine App bestätigt: `POST /api/v1/pair/approve` mit `{"id":"…"}` (mit Token) → `204`.
     4. Die neue App ruft `POST /api/v1/pair` erneut auf → `200 {"token":"…"}` (einmalig).
     Ohne Bestätigung verfällt die Anfrage nach 60 s.
3. Alle weiteren Aufrufe mit Header `Authorization: Bearer <token>` (WebSocket: `?token=`).
4. Nur im **Entwicklermodus** der Firmware bestätigt zusätzlich die BOOT-Taste des DevKits.

Max. 8 gekoppelte Apps; die älteste fällt heraus.

## HTTP

| Methode | Pfad | Zweck |
|---|---|---|
| GET | `/api/v1/info` | Gerät: id, name, fw, Maschine gekoppelt?, Speicher frei |
| GET | `/api/v1/state` | aktueller Maschinenstatus (wie WS `status`, geräteneutrale Feldnamen) |
| POST | `/api/v1/machine/program` | Einzelwerte senden `{"temperature":°C,"time":s,"speed":n}` |
| POST | `/api/v1/machine/stop` | Programm stoppen |
| POST | `/api/v1/sequence/start` | Programm starten `{"programId":"…"}` oder inline `{"name":"…","steps":[…]}` |
| POST | `/api/v1/sequence/abort` | laufendes Programm abbrechen (stoppt auch die Maschine) |
| GET | `/api/v1/sequence` | Zustand des Programms (wie WS `seq`), z. B. nach dem Wiederverbinden der App |
| GET | `/api/v1/recipes?since=<rev>` | Änderungen seit Revision: `{"rev":n,"changed":[…],"deleted":[ids]}` |
| GET | `/api/v1/recipes/{id}` | ein Rezept |
| PUT | `/api/v1/recipes/{id}` | anlegen/ändern, Header `If-Match: <rev>` gegen Überschreiben |
| DELETE | `/api/v1/recipes/{id}` | löschen |
| GET | `/api/v1/media/{sha256}` | Foto (WebP), unveränderlich, cachebar |
| PUT | `/api/v1/media/{sha256}` | Foto hochladen (max. 1 MB) |
| GET | `/api/v1/programs` | Programme (feste Abläufe) |
| POST | `/api/v1/machine/onboard` | Maschine koppeln (Ablauf siehe architektur.md) |
| POST | `/api/v1/ota` | Firmware-Update hochladen (siehe [`updates.md`](updates.md)) |
| POST | `/api/v1/pair` | Kopplung anfragen (siehe oben) |
| POST | `/api/v1/pair/approve` | Kopplungsanfrage bestätigen `{"id"}` |
| POST | `/api/v1/reset` | Werksreset (siehe unten), danach Neustart |
| POST | `/api/v1/time` | Uhrzeit von der App setzen (die Brücke hat kein Internet/NTP) |

### `GET /api/v1/info`

```json
{
  "id": "98C0",
  "name": "Rührlink",
  "fw": "0.1.0",
  "api": 1,
  "storage": {"backend": "sd", "totalKB": 30000000, "freeKB": 29000000},
  "appliance": {
    "paired": true,
    "connected": true,
    "caps": {
      "model": "kenwood-kcl95",
      "name": "Cooking Chef XL",
      "tempMin": 20, "tempMax": 180,
      "speeds": [
        {"v": 3, "label": "4", "group": "stir"},
        {"v": 4, "label": "1", "group": "stir"},
        {"v": 0, "label": "Aus", "group": "off"},
        {"v": 5, "label": "min", "group": "continuous"},
        {"v": 12, "label": "max", "group": "continuous"}
      ],
      "timeMaxS": 28800,
      "hotTemp": 60, "hotSpeedMax": 7,
      "hasScale": true,
      "remoteStart": false
    }
  }
}
```

### Einzelheiten

- `POST /api/v1/reset` – Antwort `202`, nach 0,5 s Werksreset und Neustart. Gelöscht: Heim-WLAN, alle
  App-Tokens, Passwort des Maschinen-WLANs (Maschine muss neu gekoppelt werden). Bleibt: Einrichtungs-Code
  (QR auf dem Etikett) und Rezepte. Ohne App: Netzteil 5× kurz hintereinander aus/ein (< 10 s an).

- `POST /api/v1/time` – Body `{"epoch": 1794500000, "tz": "Europe/Berlin"}` (Unix-Sekunden, IANA-Zone).
  Antwort `204`. Die App schickt das bei jedem Verbinden.
- `POST /api/v1/machine/stop` – kein Body, Antwort `202`. Die Brücke sendet intern `time = 1`;
  die Maschine bestätigt das wie jedes `setProgramme` → WS `ack`.
- `POST /api/v1/machine/program` – Antwort `202` sofort; ob die Maschine übernommen hat,
  zeigt das WS-Ereignis `ack` (bei Ausbleiben nach 5 s: `{"t":"ack","d":{"cmd":"setProgramme","ok":false}}`).
  `400` bei Werten außerhalb von `caps`, `409` wenn keine Maschine verbunden ist.
- `POST /api/v1/machine/onboard` – kein Body, Antwort `202`. Fortschritt per WS:
  `{"t":"onboard","d":{"state":"searching|sent|done|failed","reason":null}}`.
  `done` kommt erst, wenn sich die Maschine tatsächlich mit der Brücke verbunden hat (max. 3 min).
  **Während der Kopplung ist die Brücke im Heim-WLAN nicht erreichbar** (ca. 10 s bis 3 min): die STA geht ins
  Setup-WLAN der Maschine und hält danach den Kanal. Die App zeigt den Schritt an und verbindet danach neu;
  Ergebnis dann über `GET /info` (`appliance.connected`). Die WS-Meldungen `searching`/`sent` gehen evtl. verloren.

- `POST /api/v1/sequence/start` – Body entweder `{"programId":"eischnee"}` (Datei `/data/programs/<id>.json`)
  oder inline `{"name":"Pizzateig","steps":[{"speed":7,"time":60,"temperature":0,"label":"Unterrühren"}, …]}`
  (max. 20 Schritte, Summe ≤ `timeMaxS`). Antwort `202` mit dem Zustand (wie `GET /sequence`).
  Die Brücke sendet Schritt 1; **gestartet wird am Gerät** (Knopf drücken, Zustand `waiting_start`), danach
  laufen alle Schritte ohne weiteren Knopfdruck (Werte werden live nachgeschoben).
  Fehler: `400 {"error":"empty|too_many|range|hot_speed|total_time|first_step","step":<index ab 0>}`,
  `404 {"error":"program"}`, `409 {"error":"not_connected|busy|not_ready"}` (`not_ready` = Schüssel fehlt
  oder Kopf offen).
- `POST /api/v1/ota` – Body = Firmware-Image (`application/octet-stream`, mit `Content-Length`), Header
  `X-Sha256: <hex>` und `X-Signature: <hex, 64 Byte r||s>` aus dem `manifest.json` der App. Antwort `202`, nach 1 s
  Neustart in die neue Firmware. Die Brücke ist während des Uploads (ca. 30 s) für andere Anfragen blockiert –
  den Fortschritt zählt die App selbst (gesendete Bytes). Fehler: `400 {"error":"header|upload|size|sha256|signature|image"}`,
  `409 {"error":"busy"}` (Programm oder Maschine läuft), `413 {"error":"size"}`. Danach `GET /info` → `fw` prüfen.
- `POST /api/v1/sequence/abort` – Antwort `202`; `409 {"error":"idle"}`, wenn nichts läuft.
- Während ein Programm läuft, antworten `POST /machine/program` mit `409 {"error":"busy"}`;
  `POST /machine/stop` bricht das Programm ab (wie `/sequence/abort`).
- `GET /api/v1/programs` – `{"programs":[{"id":"eischnee","name":"Eischnee","steps":[…]}]}` aus
  `/data/programs/*.json`. Programme anlegen geht vorerst nur inline über `/sequence/start`.

### Rezepte und Fotos (Stand 15.11.)

Die Brücke ist die Quelle der Wahrheit, jede App hält eine Kopie und gleicht sie über Revisionen ab.
Jede Änderung (Anlegen, Ändern, Löschen) bekommt die nächste **Bibliotheks-Revision** `rev` (1, 2, 3 …);
das gespeicherte Rezept trägt sie als eigenes `rev`.

- `GET /api/v1/recipes?since=<rev>` – `{"rev":<aktuell>,"changed":[<ganze Rezepte mit rev > since>],"deleted":["<id>",…]}`.
  Ohne `since` (bzw. `0`) kommen alle Rezepte und keine Löschungen. Ungeordnet. Die Antwort wird gestreamt
  (`Transfer-Encoding: chunked`) und kann groß werden – die App liest sie ganz und merkt sich danach `rev`.
  Ist `rev` der Antwort **kleiner** als der Stand der App (andere Brücke, Speicher getauscht), holt die App alles neu (`since=0`).
- `GET /api/v1/recipes/{id}` – ein Rezept, Header `ETag: "<rev>"`. `404 {"error":"recipe"}` bzw. `{"error":"deleted"}`.
- `PUT /api/v1/recipes/{id}` – Body = Rezept (JSON, **max. 16 KB**), Header `If-Match: <rev>`:
  `0` = neu (darf es noch nicht geben; ein gelöschtes Rezept zählt als nicht vorhanden),
  `n` = ändern, nur wenn die Brücke noch Revision `n` hat; ohne Header wird ohne Prüfung überschrieben
  (nur für „trotzdem speichern“ gedacht – die App schickt immer `If-Match`).
  `id` = Kleinbuchstaben, Ziffern, `-`, max. 40 Zeichen (die App nimmt eine UUID v4).
  Die Brücke setzt `v`, `id`, `rev` und `updated` selbst (`updated` aus ihrer Uhr, ohne gestellte Uhr wird das der App
  übernommen) und behält unbekannte Felder. Antwort `200 {"id","rev","updated"}`, danach WS `recipes`.
  Fehler: `412 {"error":"rev","rev":<aktuell, 0 = gibt es nicht>,"deleted":bool}` (inzwischen geändert/gelöscht),
  `400 {"error":"id|if_match|body|json|title|servings|photo|tags|ingredients|steps"}`, `413 {"error":"size"}`,
  `503 {"error":"memory"}` (Brücke hat gerade zu wenig RAM, später noch einmal), `500 {"error":"storage"}`.
- `DELETE /api/v1/recipes/{id}` – `If-Match` wie oben (optional). Antwort `200 {"id","rev","deleted":true}`, WS `recipes`;
  `404`, `412` wie oben. Gelöschte Rezepte bleiben als Löschvermerk (Tombstone) stehen, damit andere Apps sie über
  `since` erfahren. (Aufräumen alter Tombstones: später.)
- `PUT /api/v1/media/{sha256}` – Body = Foto als **WebP** (`Content-Type: image/webp`, mit `Content-Length`, **max. 1 MB**),
  `{sha256}` = SHA-256 des Inhalts, 64 Hex-Zeichen klein. Die Brücke schreibt in Blöcken direkt auf den Speicher,
  prüft Größe, WebP-Kennung (`RIFF…WEBP`) und Prüfsumme und macht das Foto erst dann sichtbar. Antwort
  `201 {"sha256"}`, `200` wenn es schon da war. Fehler: `400 {"error":"sha256|format|size|upload"}`, `413 {"error":"size"}`,
  `507 {"error":"full"}` (Speicher fast voll), `500 {"error":"storage"}`. Während des Uploads (ca. 1–3 s) wartet die API.
  Reihenfolge in der App: erst das Foto hochladen, dann das Rezept speichern, das darauf verweist.
- `GET /api/v1/media/{sha256}` – das Foto (`image/webp`), unveränderlich: die App speichert es dauerhaft zwischen.
  `404 {"error":"media"}`. Nicht mehr benutzte Fotos werden vorerst nicht gelöscht.

Begrenzungen wegen des knappen RAM (ESP32-C6 ohne PSRAM): Ein Rezept wird ganz gelesen und geprüft (≤ 16 KB +
cJSON-Baum, sofort wieder freigegeben), alles andere wird gestreamt (Blöcke von 1–2 KB).

`speeds` = wählbare Stufen in der Reihenfolge am Gerät (`group`: off/stir/continuous); gesendet wird `speed = v`,
der Status meldet `speedSet = v`. Unbekannter Wert → App zeigt die Zahl.
`hotTemp`/`hotSpeedMax`: Ab `hotTemp` °C (Soll- oder Ist-Temperatur) sind in Programmen nur Dauerstufen bis
einschließlich `hotSpeedMax` erlaubt (Rührintervalle immer) – die App graut höhere Stufen aus.

**Die App baut Regler und Grenzen aus `caps`** – nie fest einprogrammiert. So funktioniert
ein weiteres Gerät ohne App-Update. Werte der KCL95 sind vorläufig und werden an der Maschine geprüft.

## WebSocket `/api/v1/ws`

Brücke → App:

```json
{"t":"status","d":{"connected":true,"running":false,"active":false,"keepWarm":false,"heating":false,"hot":false,"bowlFitted":true,"lidClosed":true,"bowlTemp":23,"tempSet":0,"speedSet":0,"remainingS":0}}
{"t":"weight","d":{"g":251.4}}
{"t":"seq","d":{"programId":"dinkelteig","name":"Dinkelteig kneten","step":2,"of":4,"label":"Kneten","state":"running","reason":null,"stepRemainingS":240}}
{"t":"ack","d":{"cmd":"setProgramme","ok":true}}
{"t":"recipes","d":{"rev":42}}
```

- `status` ist **geräteneutral**: `running` (Motor dreht – bei Rührintervallen im Wechsel an/aus), `active`
  (Programm der Maschine läuft, auch Heizen ohne Motor und Intervallpausen; für „läuft das Programm?“),
  `keepWarm` (Warmhalten, das die KCL95 nach einem Heizprogramm von selbst startet: 30 min, Rührintervall 2;
  `remainingS` zählt dann die Warmhaltezeit; beenden mit `POST /machine/stop`), `lidClosed` (Kopf/Deckel verriegelt),
  `bowlTemp`/`tempSet` in °C, `speedSet`, `remainingS` in Sekunden. Gerätespezifische Namen bleiben im Treiber.
- `seq` kommt bei jeder Änderung von Zustand/Schritt, bei Schritten ohne Motor zusätzlich 1×/s.
  `step` zählt ab 1 (`0` = kein Programm). `programId` ist `null` bei Inline-Programmen.
  `state`: `idle` · `waiting_start` (Knopf an der Maschine drücken) · `running` · `paused` · `done` · `aborted`.
  `reason`: `null` · `press_start` · `lid_open` · `bowl_missing` · `machine_paused` (am Gerät angehalten) ·
  `disconnected` · `no_ack` (Maschine bestätigt nicht) · `user` (Abbruch per App) · `timeout` (30 min pausiert).
- `weight` max. 10 Nachrichten/s.
- `recipes` signalisiert, dass sich die Bibliothek geändert hat → App holt `GET /recipes?since=`.

App → Brücke: dieselben Befehle wie HTTP, z. B. `{"t":"program","d":{"temperature":0,"time":120,"speed":2}}`.

## Datenformate

### Rezept

```json
{
  "v": 1,
  "id": "uuid",
  "rev": 7,
  "updated": "2026-11-12T10:00:00Z",
  "title": "Dinkel-Brot",
  "servings": 4,
  "photo": "sha256",
  "tags": ["Brot", "Teig"],
  "ingredients": [{"amount": 500, "unit": "g", "name": "Dinkelmehl 630"}],
  "steps": [
    {"text": "Mehl in die Schüssel wiegen.", "photo": null, "weigh": {"target": 500, "unit": "g"}},
    {"text": "Kneten.", "machine": {"programId": "dinkelteig"}},
    {"text": "Kurz rühren.", "machine": {"temperature": 0, "time": 60, "speed": 2}}
  ]
}
```

Pflicht ist nur `title` (1–200 Zeichen). `servings` 1–1000, `photo` = SHA-256 eines Fotos oder `null`.
Ein Schritt hat `text` und optional **entweder** `weigh` (Zielmenge, `unit` meist `g`; skaliert die App mit den Portionen)
**oder** `machine` (`programId` eines Programms der Brücke **oder** Einzelwerte wie bei `/machine/program`).
Mengen in `ingredients` beziehen sich auf `servings`. `amount` 0 = ohne Mengenangabe („Salz“).

### Programm

```json
{
  "id": "dinkelteig",
  "name": "Dinkelteig kneten",
  "scaleBy": {"ingredient": "flour", "baseGrams": 500},
  "steps": [
    {"speed": 1, "time": 60,  "temperature": 0, "label": "Unterrühren"},
    {"speed": 2, "time": 300, "temperature": 0, "label": "Kneten"},
    {"speed": 0, "time": 600, "temperature": 0, "label": "Ruhen"},
    {"speed": 2, "time": 120, "temperature": 0, "label": "Nachkneten"}
  ]
}
```

Die Werte im Beispiel sind Platzhalter – echte Werte werden an der Maschine ermittelt.
Ob eine Ruhephase per `speed: 0` im laufenden Programm funktioniert, ist **noch zu testen**.

### Assistenten

Assistenten (z. B. Pizzateig) sind **Code in der App**, nicht Teil dieser API (Entscheidung 12.11.):
Sie fragen nach, berechnen Zutaten und erzeugen ein Programm, das die App inline an
`POST /api/v1/sequence/start` schickt. Die Brücke prüft es wie jedes andere Programm.
