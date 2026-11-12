# Lokale App-API der Freikoch Brücke (v1, Entwurf)

Die API ist nur im Heim-WLAN erreichbar. Sie ist die einzige Schnittstelle zwischen
Brücke und App und wird in beiden Repos (`freikoch_esp`, `freikoch_app`) gegen diese Datei
entwickelt. Änderungen hier zuerst, dann im Code.

## Finden

- mDNS-Dienst `_freikoch._tcp`, Port 80
- TXT-Records: `id=<geräte-id>`, `fw=<version>`, `api=1`, `name=<anzeigename>`

## Koppeln & Anmelden

1. App ruft `POST /api/v1/pair` auf → Brücke blinkt blau, Antwort `202 {"pairing":"waiting"}`.
2. Nutzerin drückt **die Taste an der Brücke** (innerhalb 60 s).
3. Erneuter Aufruf `POST /api/v1/pair` → `200 {"token":"<zufällig, 32 Byte base64>"}`.
4. Alle weiteren Aufrufe mit Header `Authorization: Bearer <token>` (WebSocket: `?token=`).

Bei Einrichtung per Bluetooth erhält die App den Token direkt im Provisioning-Ablauf.

## HTTP

| Methode | Pfad | Zweck |
|---|---|---|
| GET | `/api/v1/info` | Gerät: id, name, fw, Maschine gekoppelt?, Speicher frei |
| GET | `/api/v1/state` | aktueller Maschinenstatus (wie WS `status`) |
| POST | `/api/v1/machine/program` | Einzelwerte senden `{"temperature":°C,"time":s,"speed":n}` |
| POST | `/api/v1/machine/stop` | Programm stoppen |
| POST | `/api/v1/sequence/start` | Multi-Programm laden `{"programId":"…"}` oder inline `{"steps":[…]}` |
| POST | `/api/v1/sequence/abort` | Multi-Programm abbrechen |
| GET | `/api/v1/recipes?since=<rev>` | Änderungen seit Revision: `{"rev":n,"changed":[…],"deleted":[ids]}` |
| GET | `/api/v1/recipes/{id}` | ein Rezept |
| PUT | `/api/v1/recipes/{id}` | anlegen/ändern, Header `If-Match: <rev>` gegen Überschreiben |
| DELETE | `/api/v1/recipes/{id}` | löschen |
| GET | `/api/v1/media/{sha256}` | Foto (WebP), unveränderlich, cachebar |
| PUT | `/api/v1/media/{sha256}` | Foto hochladen (max. 1 MB) |
| GET | `/api/v1/programs` | Multi-Programm-Presets |
| POST | `/api/v1/machine/onboard` | Maschine koppeln (Ablauf siehe architektur.md) |
| POST | `/api/v1/ota` | Firmware-Update hochladen (siehe [`updates.md`](updates.md)) |
| POST | `/api/v1/time` | Uhrzeit von der App setzen (die Brücke hat kein Internet/NTP) |

## WebSocket `/api/v1/ws`

Brücke → App:

```json
{"t":"status","d":{"bowlTemperature":23,"speedSetting":0,"remainingTime":0,"motorOn":false,"bowlFitted":true,"HeadLiftInterlockClosed":true,"connected":true}}
{"t":"weight","d":{"g":251.4}}
{"t":"seq","d":{"programId":"dinkelteig","step":2,"of":4,"state":"running|waiting_start|paused|done|aborted","reason":null}}
{"t":"ack","d":{"cmd":"setProgramme","ok":true}}
{"t":"recipes","d":{"rev":42}}
```

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

### Multi-Programm

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
