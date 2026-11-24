// Rührlink – Einstiegspunkt
//
// main.c verdrahtet nur die Module (components/*) und enthält keine eigene Logik.
// Modulübersicht: docs/architektur.md

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "api.h"
#include "appliance.h"
#include "diag.h"
#include "factory.h"
#include "kenwood.h"
#include "led.h"
#include "net.h"
#include "provision.h"
#include "recipes.h"
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

#if CONFIG_RL_RAM_LOG
static void alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    ESP_LOGE("ram", "FEHLGESCHLAGEN: %u Byte (caps 0x%lx) in %s, frei intern %u, groesster Block %u",
             (unsigned)size, (unsigned long)caps, function_name ? function_name : "?",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void log_ram(const char *wann)
{
    ESP_LOGI("ram", "[%s] intern frei %u KB, Tiefststand %u KB, groesster Block %u KB",
             wann,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}
#endif

void app_main(void)
{
    diag_init();  // zuerst: ab hier landen alle Log-Ausgaben auch im Diagnose-Puffer
    ESP_LOGI(TAG, "Ruehrlink startet");
#if CONFIG_RL_RAM_LOG
    heap_caps_register_failed_alloc_callback(alloc_failed);
    log_ram("Start");
#endif
    init_nvs();
    update_init();
    factory_check_power_cycles();
    led_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (storage_init() == ESP_OK) {
        recipes_init();
    } else {
        // Ohne Speicher läuft die Brücke weiter (manuelle Steuerung), nur ohne Rezepte.
        ESP_LOGE(TAG, "Speicher nicht verfuegbar");
    }

    net_start();
    appliance_register(kenwood_driver());
    sequencer_init();
    api_start();
    provision_start();

#if CONFIG_RL_RAM_LOG
    log_ram("alles gestartet");
#endif
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
#if CONFIG_RL_RAM_LOG
        log_ram("laufend");
#endif
    }
}
