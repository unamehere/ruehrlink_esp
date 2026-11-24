#include "strip.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "strip";

static led_strip_handle_t s_strip;
static bool s_powered;

static void power(bool on)
{
#if CONFIG_RL_STRIP_EN_GPIO >= 0
    gpio_set_level(CONFIG_RL_STRIP_EN_GPIO, on);
#endif
    if (on && !s_powered) vTaskDelay(pdMS_TO_TICKS(5));  // Soft-Start der 5 V abwarten, bevor Daten kommen
    s_powered = on;
}

void strip_init(void)
{
#if CONFIG_RL_STRIP_EN_GPIO >= 0
    gpio_reset_pin(CONFIG_RL_STRIP_EN_GPIO);
    gpio_set_direction(CONFIG_RL_STRIP_EN_GPIO, GPIO_MODE_OUTPUT);
#endif
    power(false);
    const led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_RL_STRIP_DATA_GPIO,
        .max_leds = CONFIG_RL_STRIP_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
#if CONFIG_RL_STRIP_INVERT
        .flags.invert_out = true,
#endif
    };
    const led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "Schuesselbeleuchtung nicht verfuegbar");
        s_strip = NULL;
    }
}

void strip_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    if (!r && !g && !b) {
        led_strip_clear(s_strip);
        power(false);
        return;
    }
    power(true);
    for (int i = 0; i < CONFIG_RL_STRIP_LEDS; i++) led_strip_set_pixel(s_strip, i, r, g, b);
    led_strip_refresh(s_strip);
}
