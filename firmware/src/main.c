// Rührlink – Einstiegspunkt
//
// main.c verdrahtet nur die Module (components/*) und enthält keine eigene Logik.
// Modulübersicht: docs/architektur.md

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "api.h"
#include "appliance.h"
#include "factory.h"
#include "kenwood.h"
#include "led.h"
#include "net.h"
#include "provision.h"
#include "sequencer.h"
#include "storage.h"
#include "update.h"

static const char *TAG = "ruehrlink";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Vorläufiger Test, dass der Speicher Neustarts übersteht. Fliegt raus, sobald die
// Rezeptablage steht.
static void storage_smoke_test(void)
{
    const char *path = STORAGE_ROOT "/boot_count.txt";
    char *old = storage_read(path, 32, NULL);
    int count = old ? atoi(old) : 0;
    free(old);
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", count + 1);
    if (storage_write(path, buf, n) == ESP_OK) {
        ESP_LOGI(TAG, "Start Nr. %d (aus %s)", count + 1, path);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ruehrlink startet");
    init_nvs();
    update_init();
    factory_check_power_cycles();
    led_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (storage_init() == ESP_OK) {
        storage_smoke_test();
    } else {
        // Ohne Speicher läuft die Brücke weiter (manuelle Steuerung), nur ohne Rezepte.
        ESP_LOGE(TAG, "Speicher nicht verfuegbar");
    }

    net_start();
    appliance_register(kenwood_driver());
    sequencer_init();
    api_start();
    provision_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
