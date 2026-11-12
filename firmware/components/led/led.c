#include "led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static volatile led_signal_t s_sig = LED_OFF;
static volatile uint8_t s_level = 24;   // dezent; Küche
static TaskHandle_t s_task;

static void show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    uint32_t l = s_level;
    led_strip_set_pixel(s_strip, 0, r * l / 255, g * l / 255, b * l / 255);
    led_strip_refresh(s_strip);
}

// Wartet bis zu `ms`, kehrt früher zurück, wenn ein neues Signal kommt.
static bool wait(uint32_t ms) { return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) == 0; }

static void led_task(void *arg)
{
    while (1) {
        led_signal_t sig = s_sig;
        switch (sig) {
        case LED_PAIRING:
        case LED_SETUP: {
            uint8_t r = sig == LED_SETUP ? 255 : 0, g = sig == LED_SETUP ? 160 : 0, b = sig == LED_SETUP ? 0 : 255;
            show(r, g, b);
            if (!wait(400)) continue;
            show(0, 0, 0);
            if (!wait(400)) continue;
            break;
        }
        case LED_OK:
            show(0, 255, 0);
            if (wait(2000) && s_sig == LED_OK) s_sig = LED_OFF;
            break;
        case LED_ERROR:
            for (int i = 0; i < 3 && s_sig == LED_ERROR; i++) {
                show(255, 0, 0);
                if (!wait(250)) break;
                show(0, 0, 0);
                if (!wait(250)) break;
            }
            if (s_sig == LED_ERROR) s_sig = LED_OFF;
            break;
        case LED_OFF:
        default:
            show(0, 0, 0);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            break;
        }
    }
}

void led_init(void)
{
    const led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_FK_LED_GPIO,
        .max_leds = 1,
    };
    const led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "LED nicht verfuegbar");
        s_strip = NULL;
    }
    xTaskCreate(led_task, "led", 2048, NULL, 2, &s_task);
}

void led_signal(led_signal_t s)
{
    s_sig = s;
    if (s_task) xTaskNotifyGive(s_task);
}

void led_set_brightness(uint8_t level)
{
    s_level = level;
    if (s_task) xTaskNotifyGive(s_task);
}
