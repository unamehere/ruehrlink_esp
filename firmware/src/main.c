// Freikoch Brücke – Einstiegspunkt
//
// Geplante Module (siehe docs/architektur.md):
//   net_ap       eigenes WLAN für die Maschine (IP 8.8.8.8), DNS-Server
//   net_home     Heim-WLAN (STA), mDNS _freikoch._tcp
//   machine      TLS-Server 443: /devices/*, WebSocket, setProgramme, Keepalive
//   sequencer    mehrstufige Programme mit Sicherheitsregeln
//   api          lokale App-API (HTTP + WebSocket), siehe docs/api.md
//   storage      microSD: Rezepte, Fotos, Programme
//   provision    Einrichtung per Bluetooth
//   mqtt_ha      optional: Home Assistant

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "freikoch";

void app_main(void)
{
    ESP_LOGI(TAG, "Freikoch Brücke startet");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
