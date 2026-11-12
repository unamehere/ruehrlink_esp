// net – WLAN der Brücke
//
// Zwei Schnittstellen gleichzeitig auf einem Funkteil (gleicher Kanal):
//   AP   "Freikoch-XXXX" nur für die Maschine, IP 8.8.8.8, DNS beantwortet alles mit 8.8.8.8
//   STA  Heim-WLAN für die App, Hostname freikoch-xxxx, mDNS-Dienst _freikoch._tcp
//
// Zugangsdaten fürs Heim-WLAN setzt `provision` (Bluetooth). Ohne Zugangsdaten läuft nur der AP.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_AP_IP "8.8.8.8"  // Maschine fragt DNS fest bei 8.8.8.8

// WLAN, AP, DNS und (falls eingerichtet) Heim-WLAN starten.
esp_err_t net_start(void);

// Kurze Geräte-ID aus der MAC, z. B. "98C0" – Teil von SSID, Hostname, BLE-Name.
const char *net_device_id(void);

// Daten des Maschinen-WLANs (für die Kopplung der Maschine).
const char *net_ap_ssid(void);
const char *net_ap_password(void);

// Heim-WLAN setzen (speichert dauerhaft) und verbinden.
esp_err_t net_home_set(const char *ssid, const char *password);
bool net_home_connected(void);
bool net_home_configured(void);

// Heim-WLAN für max. `seconds` trennen, damit der AP-Kanal stabil bleibt
// (Kopplung der Maschine, sonst E34). 0 = sofort wieder verbinden.
void net_home_pause(uint32_t seconds);

// Kopplung eines Geräts: STA vorübergehend mit dem offenen WLAN verbinden, dessen Name mit
// `prefix` beginnt (stärkstes Signal). Blockiert bis verbunden oder Timeout.
esp_err_t net_temp_join(const char *prefix, char *ssid_out, size_t ssid_len, uint32_t timeout_s);
// Zurück ins Heim-WLAN, vorher `hold_s` Sekunden Kanal halten (Gerät verbindet sich mit dem AP).
void net_temp_leave(uint32_t hold_s);
// Entwickler-Überschreibung des Maschinen-WLANs entfernen → wieder "Freikoch-XXXX".
void net_ap_use_default(void);

// Heim-WLAN und mDNS-Eintrag ergänzen, sobald bekannt (z. B. Firmware-Version).
void net_mdns_set_txt(const char *key, const char *value);
