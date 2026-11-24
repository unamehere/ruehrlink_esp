#include "led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "driver/ledc.h"
#include "strip.h"

static const char *TAG = "led";

static volatile led_signal_t s_sig = LED_OFF;
static volatile uint8_t s_level = 24;   // dezent; Küche
static volatile bool s_connected;
static TaskHandle_t s_task;

// ---------------------------------------------------------------------------
// Ausgabe: rot + grün über LEDC

#define LED_RES   LEDC_TIMER_13_BIT   // fein genug für die stark gedimmte Daueranzeige
#define LED_MAX   8191
#define CH_R      LEDC_CHANNEL_0
#define CH_G      LEDC_CHANNEL_1

static bool s_ok;

static void set_duty(ledc_channel_t ch, uint32_t v, uint32_t scale_pct)
{
    // v 0…255 (Farbanteil), Helligkeit s_level 0…255, scale_pct gleicht die LED-Farben an
    uint32_t d = (uint32_t)((uint64_t)v * s_level * LED_MAX * scale_pct / (255u * 255u * 100u));
    if (d == 0 && v && s_level) d = 1;  // gedimmt, aber nie unsichtbar
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, d);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

// Blau gibt es nicht: Blauanteil wird grün angezeigt (Kopplung = grün blinkt).
static void show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ok) return;
    set_duty(CH_R, r, 100);
    set_duty(CH_G, g > b ? g : b, CONFIG_RL_LED_G_SCALE);
}

static void hw_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LED_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) {
        ESP_LOGW(TAG, "LED-Timer nicht verfuegbar");
        return;
    }
    const int pins[2] = {CONFIG_RL_LED_R_GPIO, CONFIG_RL_LED_G_GPIO};
    const ledc_channel_t chs[2] = {CH_R, CH_G};
    for (int i = 0; i < 2; i++) {
        const ledc_channel_config_t c = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = chs[i],
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        if (ledc_channel_config(&c) != ESP_OK) {
            ESP_LOGW(TAG, "LED-Kanal %d nicht verfuegbar", i);
            return;
        }
    }
    s_ok = true;
}

// ---------------------------------------------------------------------------
// Signale: nur kurz anzeigen, danach aus (Niklas 24.11.: LED nie dauerhaft an)

// Wartet bis zu `ms`, kehrt früher zurück, wenn ein neues Signal kommt.
static bool wait(uint32_t ms) { return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) == 0; }

// Grundzustand: aus, oder grün stark gedimmt, solange die Maschine verbunden ist
static void idle(void)
{
    uint32_t g = s_connected ? 255u * CONFIG_RL_LED_CONNECTED_PCT / 100 : 0;
    show(0, g, 0);
}

// n-mal blinken; false, wenn ein neues Signal dazwischenkam
static bool blink(uint8_t r, uint8_t g, uint8_t b, int n, uint32_t on_ms, uint32_t off_ms)
{
    for (int i = 0; i < n; i++) {
        show(r, g, b);
        if (!wait(on_ms)) return false;
        show(0, 0, 0);
        if (!wait(off_ms)) return false;
    }
    return true;
}

static void led_task(void *arg)
{
    while (1) {
        led_signal_t sig = s_sig;
        bool done = true;
        switch (sig) {
        case LED_PAIRING:
            done = blink(0, 0, 255, 3, 150, 150);
            break;
        case LED_SETUP:
            done = blink(255, 160, 0, 3, 300, 300);
            break;
        case LED_OK:
            show(0, 255, 0);
            done = wait(1000);
            show(0, 0, 0);
            break;
        case LED_ERROR:
            done = blink(255, 0, 0, 3, 250, 250);
            break;
        case LED_OFF:
        default:
            idle();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        // Signal abgespielt → aus (außer es kam inzwischen ein neues)
        if (done && s_sig == sig) s_sig = LED_OFF;
    }
}

void led_init(void)
{
    strip_init();   // Schüsselbeleuchtung: 5 V aus, bis sie gebraucht wird
    hw_init();
    xTaskCreate(led_task, "led", 2048, NULL, 2, &s_task);
}

void led_signal(led_signal_t s)
{
    s_sig = s;
    if (s_task) xTaskNotifyGive(s_task);
}

void led_set_connected(bool on)
{
    s_connected = on;
    if (s_task) xTaskNotifyGive(s_task);
}

void led_set_brightness(uint8_t level)
{
    s_level = level;
    if (s_task) xTaskNotifyGive(s_task);
}
