// provision – Einrichtung per Bluetooth (ESP-IDF network_provisioning, Security 2 / SRP6a)
//
// Aktiv, solange kein Heim-WLAN eingerichtet ist, sonst 10 min nach dem Einschalten
// (so kann ein weiteres Handy per QR-Code koppeln: Brücke kurz aus- und einstecken).
// Der Einrichtungs-Code (PoP) wird beim ersten Start pro Gerät erzeugt und steht als
// QR-Code auf dem Etikett. Ablauf und QR-Format: docs/architektur.md.
//
// Zusätzlicher Endpunkt "freikoch": liefert der App Token, Geräte-ID und Hostnamen.
#pragma once

#include "esp_err.h"

esp_err_t provision_start(void);
