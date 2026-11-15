// interne Hilfe des Moduls kenwood: Waage der KCL95 (siehe scale.c)
#pragma once
#include <stdint.h>
#include "esp_err.h"

// Adresse der Maschine (IPv4, Netzwerk-Byte-Reihenfolge), sobald sie mit uns verbunden ist; 0 = unbekannt.
void kenwood_scale_set_host(uint32_t ip);

// Waage für `seconds` Sekunden ab jetzt brauchen (verlängert, kürzt nie). Startet die Verbindung bei Bedarf.
esp_err_t kenwood_scale_watch(uint32_t seconds);

// Gemerktes Zertifikat der Waage vergessen (neue Maschine gekoppelt).
void kenwood_scale_forget(void);

// Verbindung zur Maschine sofort prüfen (Ping, ohne Antwort binnen 2 s getrennt) – kenwood.c
void kenwood_check_alive(void);
