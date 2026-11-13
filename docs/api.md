# Lokale App-API der Freikoch Brücke (v1, Entwurf)

Die API ist nur im Heim-WLAN erreichbar. Sie ist die einzige Schnittstelle zwischen
Brücke und App und wird in beiden Repos (`freikoch_esp`, `freikoch_app`) gegen diese Datei
entwickelt. Änderungen hier zuerst, dann im Code.

## Finden

- mDNS-Dienst `_freikoch._tcp`, Port 80
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
  "name": "Freikoch Brücke",
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
- `POST /api/v1/sequence/abort` – Antwort `202`; `409 {"error":"idle"}`, wenn nichts läuft.
- Während ein Programm läuft, antworten `POST /machine/program` mit `409 {"error":"busy"}`;
  `POST /machine/stop` bricht das Programm ab (wie `/sequence/abort`).
- `GET /api/v1/programs` – `{"programs":[{"id":"eischnee","name":"Eischnee","steps":[…]}]}` aus
  `/data/programs/*.json`. Programme anlegen geht vorerst nur inline über `/sequence/start`.

`speeds` = wählbare Stufen in der Reihenfolge am Gerät (`group`: off/stir/continuous); gesendet wird `speed = v`,
der Status meldet `speedSet = v`. Unbekannter Wert → App zeigt die Zahl.
`hotTemp`/`hotSpeedMax`: Ab `hotTemp` °C (Soll- oder Ist-Temperatur) sind in Programmen nur Dauerstufen bis
einschließlich `hotSpeedMax` erlaubt (Rührintervalle immer) – die App graut höhere Stufen aus.

**Die App baut Regler und Grenzen aus `caps`** – nie fest einprogrammiert. So funktioniert
ein weiteres Gerät ohne App-Update. Werte der KCL95 sind vorläufig und werden an der Maschine geprüft.

## WebSocket `/api/v1/ws`

Brücke → App:

```json
{"t":"status","d":{"connected":true,"running":false,"heating":false,"hot":false,"bowlFitted":true,"lidClosed":true,"bowlTemp":23,"tempSet":0,"speedSet":0,"remainingS":0}}
{"t":"weight","d":{"g":251.4}}
{"t":"seq","d":{"programId":"dinkelteig","name":"Dinkelteig kneten","step":2,"of":4,"label":"Kneten","state":"running","reason":null,"stepRemainingS":240}}
{"t":"ack","d":{"cmd":"setProgramme","ok":true}}
{"t":"recipes","d":{"rev":42}}
```

- `status` ist **geräteneutral**: `running` (Motor/Programm läuft), `lidClosed` (Kopf/Deckel verriegelt),
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
