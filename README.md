# Rührlink – Firmware

Die **Rührlink** ist eine kleine Box auf Basis des ESP32-S3. Sie ersetzt die
abgeschaltete Hersteller-Cloud für die Küchenmaschine *Kenwood Cooking Chef XL (KCL95)*
und macht sie wieder vernetzt nutzbar – komplett lokal, ohne Internet, ohne Konto.

> Rührlink ist ein unabhängiges Projekt und steht in keiner Verbindung zu Kenwood
> oder De'Longhi. „Kenwood“ und „Cooking Chef“ sind Marken ihrer jeweiligen Inhaber
> und werden hier nur zur Beschreibung der Kompatibilität genannt.

## Was die Brücke macht

- Spannt ein eigenes WLAN für die Maschine auf und spielt für sie die Cloud
  (DNS, TLS-WebSocket, `setProgramme`).
- Ist gleichzeitig im Heim-WLAN und bietet dort eine lokale API für die
  Rührlink-App (iOS/Android) an – siehe [`docs/api.md`](docs/api.md).
- Führt mehrstufige Programme (z. B. Teig kneten → ruhen → nachkneten) selbst aus,
  damit nichts abbricht, wenn das Handy schläft.
- Speichert die Rezeptbibliothek des Haushalts (inkl. Fotos) auf der microSD-Karte.
- Optional: Home Assistant über MQTT.

## Aufbau des Repos

| Ordner | Inhalt |
|---|---|
| `firmware/` | Firmware, ESP-IDF über PlatformIO (`pio run`) |
| `docs/` | Architektur, Maschinen-Protokoll, App-API |

## Firmware bauen

Voraussetzung: [PlatformIO](https://platformio.org/) (VS Code-Erweiterung oder CLI).

```sh
cd firmware
pio run                 # bauen
pio run -t upload       # flashen (USB)
pio device monitor      # serielle Ausgabe
```

Zielhardware für die Beta: ESP32-S3-DevKitC-1 mit N16R8 (16 MB Flash, 8 MB PSRAM) + microSD-Modul.
Die Firmware läuft damit auf frei kaufbaren Teilen – eine eigene Platine ist nicht nötig.

## Status

Frühe Entwicklung (MVP für die Beta, Dezember 2026).

## Lizenz

Noch offen (Vorschlag: GPLv3). 
