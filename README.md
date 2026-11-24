# Rührlink – Firmware

Der **Rührlink** ist eine kleine Box auf Basis des ESP32-C6. Er ersetzt die
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
- Speichert die Rezeptbibliothek des Haushalts (inkl. Fotos) auf der microSD-Karte (SPI). Ohne Karte
  läuft sie im internen Flash weiter; vorhandene Daten werden einmal auf eine leere Karte kopiert.
- Updates per Datei `ruehrlink-<version>.rlfw` (signiert, A/B-Partitionen mit automatischem Rückfall),
  unabhängig von der App-Version. Die App bringt jeweils die neueste Firmware mit.
- „Maschine neu koppeln“ ohne Werksreset; der Name des Maschinen-WLANs bleibt bei Updates gleich.
- Diagnose für „Problem melden“: letzte Log-Ausgaben im RAM, Zugangsdaten maskiert (`GET /api/v1/diag`).
- Später: Home Assistant über MQTT.

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

Zielhardware: die **Rührlink-Platine** mit ESP32-C6-WROOM-1-N8 (8 MB Flash, kein PSRAM), microSD per SPI,
Status-LED rot/grün, Buchse für eine WS2812-Schüsselbeleuchtung. Flashen und Logs über die USB-C-Buchse
(USB-Serial/JTAG, ohne BOOT-Taste). Pins stehen in den `Kconfig.projbuild` der Komponenten.

Neue Boxen einrichten (Firmware, Einrichtungs-Code, Etikett mit QR-Code): `tools/werkbank.py`
(Pakete: `tools/werkbank-requirements.txt`).

## Status

Beta-Vorbereitung (Firmware 0.6.0, Beta ab Dezember 2026). Änderungen: [CHANGELOG.md](CHANGELOG.md).

## Lizenz

Copyright (C) 2026 NMX Drive Systems GmbH

Die Firmware ist freie Software: Sie dürfen sie unter den Bedingungen der
[GNU General Public License, Version 3](LICENSE) weitergeben und/oder verändern.
Sie wird ohne jede Gewährleistung bereitgestellt; Einzelheiten stehen in der Lizenz.

Ausgenommen sind Bibliotheken Dritter (z. B. ESP-IDF) – für sie gelten ihre eigenen Lizenzen.
Der Name „Rührlink“ und das Logo sind nicht Teil der Lizenz.
