#include "factory.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "storage.h"

static const char *TAG = "factory";

#define QUICK_BOOTS     5                     // so oft kurz hintereinander einstecken
#define STABLE_AFTER_US (10 * 1000000LL)      // länger an = kein „kurzes“ Einschalten

static const char *const ERASE_NS[] = {"net", "api", "appliance"};  // "prov" (QR-Code) bleibt

static void set_counter(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("factory", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "quick", v);
    nvs_commit(h);
    nvs_close(h);
}

static void stable_cb(void *arg)
{
    set_counter(0);
}

void factory_reset(void)
{
    ESP_LOGW(TAG, "Werksreset");
    for (size_t i = 0; i < sizeof(ERASE_NS) / sizeof(ERASE_NS[0]); i++) {
        nvs_handle_t h;
        if (nvs_open(ERASE_NS[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    set_counter(0);
    esp_wifi_restore();  // gespeicherte WLAN-Zugangsdaten des Treibers löschen
    esp_restart();
}

static void reset_cb(void *arg) { factory_reset(); }

void factory_reset_later(int ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = reset_cb, .name = "factory"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

// Eigener Task statt Timer-Callback: Formatieren/Löschen dauert einige Sekunden.
static void wipe_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((int)(intptr_t)arg));
    ESP_LOGW(TAG, "Alles loeschen: Nutzerdaten, dann Werksreset");
    esp_err_t err = storage_wipe();
    if (err != ESP_OK) ESP_LOGE(TAG, "Speicher loeschen: %s", esp_err_to_name(err));
    factory_reset();
}

void factory_wipe_later(int ms)
{
    if (xTaskCreate(wipe_task, "wipe", 6144, (void *)(intptr_t)ms, 5, NULL) != pdPASS) factory_reset_later(ms);
}

void factory_check_power_cycles(void)
{
    uint8_t n = 0;
    nvs_handle_t h;
    if (nvs_open("factory", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "quick", &n);
        nvs_close(h);
    }
    n++;
    if (n >= QUICK_BOOTS) {
        ESP_LOGW(TAG, "%d-mal kurz eingeschaltet", QUICK_BOOTS);
        // WLAN ist noch nicht initialisiert: Zugangsdaten nach dem Neustart über das leere NVS weg
        set_counter(0);
        for (size_t i = 0; i < sizeof(ERASE_NS) / sizeof(ERASE_NS[0]); i++) {
            if (nvs_open(ERASE_NS[i], NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {  // Speicher des WLAN-Treibers
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "Werksreset ausgefuehrt");
        return;
    }
    set_counter(n);
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = stable_cb, .name = "stable"};
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, STABLE_AFTER_US);
}
