#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "auth";

#define MAX_TOKENS   8                       // gekoppelte Handys/Tablets
#define TOKEN_LEN    32                      // Hex-Zeichen (128 Bit)
#define PAIR_WINDOW_US (60 * 1000000LL)

typedef struct {
    char t[MAX_TOKENS][TOKEN_LEN + 1];
    uint8_t next;                            // ältester Platz wird überschrieben
} tokens_t;

static tokens_t s_tok;
static int64_t s_window_until;               // 0 = kein Kopplungsfenster offen
static char s_pending[TOKEN_LEN + 1];        // nach Tastendruck, bis die App ihn abholt

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("api", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tokens", &s_tok, sizeof(s_tok));
    nvs_commit(h);
    nvs_close(h);
}

static void new_token(char *out)
{
    for (int i = 0; i < TOKEN_LEN / 8; i++) sprintf(out + i * 8, "%08lx", (unsigned long)esp_random());
    out[TOKEN_LEN] = '\0';
}

// Wartet im Kopplungsfenster auf die Taste.
static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_FK_PAIR_BUTTON_GPIO;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_window_until || s_pending[0]) continue;
        if (esp_timer_get_time() > s_window_until) {
            s_window_until = 0;
            ESP_LOGI(TAG, "Kopplungsfenster abgelaufen");
            continue;
        }
        if (gpio_get_level(pin) == 0) {
            new_token(s_pending);
            strcpy(s_tok.t[s_tok.next], s_pending);
            s_tok.next = (s_tok.next + 1) % MAX_TOKENS;
            save();
            ESP_LOGI(TAG, "Taste gedrueckt - neue App gekoppelt");
        }
    }
}

void auth_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_tok);
    if (nvs_open("api", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "tokens", &s_tok, &len) != ESP_OK) memset(&s_tok, 0, sizeof(s_tok));
        nvs_close(h);
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_FK_PAIR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "pair_btn", 2560, NULL, 3, NULL);
}

bool auth_check(const char *token)
{
    if (!token) return false;
    if (strncmp(token, "Bearer ", 7) == 0) token += 7;
    if (strlen(token) != TOKEN_LEN) return false;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (s_tok.t[i][0] && strcmp(s_tok.t[i], token) == 0) return true;
    }
    return false;
}

bool auth_pair(char *token_out, size_t len)
{
    if (s_pending[0]) {
        snprintf(token_out, len, "%s", s_pending);
        s_pending[0] = '\0';
        s_window_until = 0;
        return true;
    }
    if (!s_window_until) {
        s_window_until = esp_timer_get_time() + PAIR_WINDOW_US;
        ESP_LOGI(TAG, "Kopplung angefragt - Taste an der Bruecke druecken (60 s)");
    }
    return false;
}
