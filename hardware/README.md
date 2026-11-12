# Hardware

## Beta (Dezember 2026)

Kein eigenes Platinen-Layout – fertige Module, damit die Kleinserie rechtzeitig steht.

| Teil | Wahl |
|---|---|
| Controller | ESP32-S3-DevKitC-1, N16R8 (16 MB Flash, 8 MB PSRAM) |
| Speicher | microSD-Modul (SPI oder SDMMC), Industrie-microSD 16–32 GB |
| Status | RGB-LED (on-board, GPIO48) |
| Taste | BOOT-Taste des DevKits (Koppeln / Reset) |
| Strom | USB-C, zugekauftes CE-Netzteil 5 V |
| Gehäuse | 3D-Druck, Antennenbereich frei |

Pinbelegung SD-Karte: wird hier festgehalten, sobald das Modul gewählt ist.

## Serie (2027)

Eigene Platine in KiCad mit ESP32-S3-WROOM-1-N16R8 (SMD), microSD oder eMMC,
USB-C, Taster, RGB-LED. Ordnerstruktur folgt:

- `kicad/` – Schaltplan und Layout
- `gehaeuse/` – STEP / STL
- `fertigung/` – Gerber, BOM, Bestückungsdaten
