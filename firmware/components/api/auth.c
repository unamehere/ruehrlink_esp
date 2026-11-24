#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "led.h"

#if CONFIG_RL_DEVELOPER_MODE
#include "driver/gpio.h"
#endif

static const char *TAG = "auth";

#define MAX_TOKENS     8                     // gekoppelte Handys/Tablets
#define TOKEN_LEN      32                    // Hex-Zeichen (128 Bit)
#define ID_LEN         16
#define REQUEST_TTL_US (60 * 1000000LL)

typedef struct {
    char t[MAX_TOKENS][TOKEN_LEN + 1];
    uint8_t next;                            // ältester Platz wird überschrieben
} tokens_t;

// Eine offene Kopplungsanfrage zur Zeit
typedef struct {
    char id[ID_LEN + 1];
    char name[32];
    int64_t expires;
    char token[TOKEN_LEN + 1];               // gesetzt, sobald bestätigt
} request_t;

static tokens_t s_tok;
static request_t s_req;
static SemaphoreHandle_t s_lock;
static auth_request_cb_t s_on_request;

static void random_hex(char *out, int len)
{
    for (int i = 0; i < len; i += 8) snprintf(out + i, 9, "%08lx", (unsigned long)esp_random());
    out[len] = '\0';
}

static void store_token(const char *t)
{
    strcpy(s_tok.t[s_tok.next], t);
    s_tok.next = (s_tok.next + 1) % MAX_TOKENS;
    nvs_handle_t h;
    if (nvs_open("api", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tokens", &s_tok, sizeof(s_tok));
    nvs_commit(h);
    nvs_close(h);
}

static bool request_open(void) { return s_req.id[0] && esp_timer_get_time() < s_req.expires; }

static void approve_locked(void)
{
    random_hex(s_req.token, TOKEN_LEN);
    store_token(s_req.token);
    led_signal(LED_OK);
    ESP_LOGI(TAG, "Kopplung bestaetigt: %s", s_req.name);
}

void auth_new_token(char *token_out, size_t len)
{
    char t[TOKEN_LEN + 1];
    random_hex(t, TOKEN_LEN);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    store_token(t);
    xSemaphoreGive(s_lock);
    snprintf(token_out, len, "%s", t);
}

#if CONFIG_RL_DEVELOPER_MODE
// Entwicklermodus: BOOT-Taste (DevKit IO0, Platine IO9) bestätigt eine offene Anfrage.
static void button_task(void *arg)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_RL_BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(CONFIG_RL_BOOT_GPIO) != 0) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (request_open() && !s_req.token[0]) approve_locked();
        xSemaphoreGive(s_lock);
    }
}
#endif

// LED aus, wenn eine Anfrage unbestätigt abläuft
static void expiry_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_req.id[0] && !request_open()) {
            if (!s_req.token[0]) led_signal(LED_OFF);
            memset(&s_req, 0, sizeof(s_req));
        }
        xSemaphoreGive(s_lock);
    }
}

void auth_init(auth_request_cb_t on_request)
{
    s_lock = xSemaphoreCreateMutex();
    s_on_request = on_request;
    nvs_handle_t h;
    size_t len = sizeof(s_tok);
    if (nvs_open("api", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "tokens", &s_tok, &len) != ESP_OK) memset(&s_tok, 0, sizeof(s_tok));
        nvs_close(h);
    }
    xTaskCreate(expiry_task, "pair_exp", 2048, NULL, 2, NULL);
#if CONFIG_RL_DEVELOPER_MODE
    xTaskCreate(button_task, "pair_btn", 2560, NULL, 3, NULL);
#endif
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

auth_pair_result_t auth_pair(const char *id, const char *name, char *id_out, size_t id_len,
                             char *token_out, size_t token_len)
{
    bool notify = false;
    auth_pair_result_t res = AUTH_PAIR_WAITING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (id && id[0] && request_open() && strcmp(id, s_req.id) == 0) {
        if (s_req.token[0]) {  // bestätigt → Token einmalig herausgeben
            snprintf(token_out, token_len, "%s", s_req.token);
            memset(&s_req, 0, sizeof(s_req));
            res = AUTH_PAIR_TOKEN;
        }
    } else {                   // neue Anfrage ersetzt eine alte
        memset(&s_req, 0, sizeof(s_req));
        random_hex(s_req.id, ID_LEN);
        snprintf(s_req.name, sizeof(s_req.name), "%s", name && name[0] ? name : "Neues Geraet");
        s_req.expires = esp_timer_get_time() + REQUEST_TTL_US;
        led_signal(LED_PAIRING);
        ESP_LOGI(TAG, "Kopplungsanfrage von \"%s\"", s_req.name);
        notify = true;
    }
    if (res == AUTH_PAIR_WAITING) snprintf(id_out, id_len, "%s", s_req.id);
    char nid[ID_LEN + 1], nname[32];
    strcpy(nid, s_req.id);
    strcpy(nname, s_req.name);
    xSemaphoreGive(s_lock);
    if (notify && s_on_request) s_on_request(nid, nname);
    return res;
}

bool auth_approve(const char *id)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (id && request_open() && strcmp(id, s_req.id) == 0 && !s_req.token[0]) {
        approve_locked();
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
