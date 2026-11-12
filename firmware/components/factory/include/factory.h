// factory – Werksreset der Brücke
//
// Löscht Heim-WLAN, App-Kopplungen und das Passwort des Maschinen-WLANs (Maschine muss neu
// gekoppelt werden). Bleibt erhalten: Einrichtungs-Code/QR (NVS "prov") und Daten auf der SD-Karte.
// Auslöser: App (POST /api/v1/reset) oder Netzteil 5× kurz hintereinander aus/ein.
#pragma once

// Direkt nach nvs_flash_init() aufrufen: zählt kurze Einschaltvorgänge, löst beim 5. den Reset aus.
void factory_check_power_cycles(void);

// Werksreset durchführen und neu starten (kehrt nicht zurück).
void factory_reset(void);

// Werksreset in `ms` Millisekunden (damit z. B. eine HTTP-Antwort noch rausgeht).
void factory_reset_later(int ms);
