# Rührlink – Firmware

Der **Rührlink** ist eine kleine Box auf Basis des ESP32-S3. Er ersetzt die
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

> **Hinweis Produkt V1:** Die eigene Rührlink-Platine (`ruehrlink_hardware`, Branch `feature/esp32-c6`) nutzt
> aus Kostengründen ein **ESP32-C6-WROOM-1-N8** (8 MB Flash, **kein PSRAM**, kein SDMMC). Für V1 muss die Firmware
> auf den C6 umgestellt werden – vorher den RAM-Bedarf (AP + STA + BLE + TLS) auf einem ESP32-C6-DevKitC-1 messen.
>
> - Build: Ziel `esp32c6`, Flash 8 MB, `partitions.csv` anpassen; `CONFIG_SPIRAM*` und
>   `MBEDTLS_EXTERNAL_MEM_ALLOC` raus, `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.
> - GPIOs: BOOT-Taster **IO9** (statt IO0, `auth.c`), Status-LED rot/grün **IO22/IO23** (LEDC, statt WS2812 an 48),
>   Streifen DATA **IO0** (RMT invertiert) / Freigabe **IO1**, microSD per SPI (`esp_vfs_fat_sdspi_mount`)
>   MISO **18** / SCK **19** / MOSI **20** / CS **21**, USB-C CC1/CC2 an ADC1 **IO2/IO3**. IO8 ist Strapping-Pin – nicht belasten.
> - Details: `ruehrlink_hardware/kicad/README.md`.

## Status

Frühe Entwicklung (MVP für die Beta, Dezember 2026).

## Lizenz

Noch offen (Vorschlag: GPLv3). 
