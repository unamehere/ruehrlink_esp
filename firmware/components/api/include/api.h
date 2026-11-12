// api – lokale App-API der Brücke (HTTP + WebSocket auf Port 80, nur Heim-WLAN)
//
// Vertrag: docs/api.md. Spricht nur mit `appliance`, nie direkt mit einem Gerätetreiber.
#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t api_start(void);

// Neuen App-Token ausstellen (Bluetooth-Einrichtung).
void api_issue_token(char *out, size_t len);
