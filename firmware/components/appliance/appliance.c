#include "appliance.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "appliance";

ESP_EVENT_DEFINE_BASE(APPLIANCE_EVENT);

static const appliance_driver_t *s_drv;
static appliance_status_t s_status;
static SemaphoreHandle_t s_lock;

esp_err_t appliance_register(const appliance_driver_t *driver)
{
    if (s_drv) return ESP_ERR_INVALID_STATE;  // MVP: genau ein Gerät pro Brücke
    s_lock = xSemaphoreCreateMutex();
    s_drv = driver;
    ESP_LOGI(TAG, "Treiber: %s (%s)", driver->caps->name, driver->caps->model);
    return driver->start();
}

const appliance_caps_t *appliance_caps(void)
{
    return s_drv ? s_drv->caps : NULL;
}

void appliance_get_status(appliance_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void appliance_set_status(const appliance_status_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&s_status, s, sizeof(*s)) != 0;
    s_status = *s;
    xSemaphoreGive(s_lock);
    if (changed) esp_event_post(APPLIANCE_EVENT, APPLIANCE_EVENT_STATUS, s, sizeof(*s), 0);
}

void appliance_report_ack(bool ok)
{
    esp_event_post(APPLIANCE_EVENT, APPLIANCE_EVENT_ACK, &ok, sizeof(ok), 0);
}

// --- Waage -------------------------------------------------------------------
// Tara rechnet die Brücke selbst (die KCL95 nimmt keinen Tara-Befehl an); alle Apps sehen dasselbe Netto.
// Die Maschine liefert ~20 Werte/s – an die Apps geht höchstens alle 100 ms ein Wert und nur bei
// Änderung ≥ 0,5 g (spätestens jede Sekunde einer, damit die App weiß, dass die Waage lebt).

#define WEIGHT_MIN_US   (100 * 1000LL)
#define WEIGHT_ALIVE_US (1000 * 1000LL)
#define WEIGHT_STEP_G   0.5f

static portMUX_TYPE s_wmux = portMUX_INITIALIZER_UNLOCKED;
static float s_gross, s_tare, s_sent_net;
static bool s_have_weight;
static int64_t s_sent_us;

static void post_weight(float gross, float net)
{
    appliance_weight_t w = {.net = net, .gross = gross};
    esp_event_post(APPLIANCE_EVENT, APPLIANCE_EVENT_WEIGHT, &w, sizeof(w), 0);
}

void appliance_report_weight(float grams)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_wmux);
    s_gross = grams;
    s_have_weight = true;
    float net = grams - s_tare;
    int64_t since = now - s_sent_us;
    float diff = net > s_sent_net ? net - s_sent_net : s_sent_net - net;
    bool send = since >= WEIGHT_MIN_US && (diff >= WEIGHT_STEP_G || since >= WEIGHT_ALIVE_US);
    if (send) {
        s_sent_us = now;
        s_sent_net = net;
    }
    portEXIT_CRITICAL(&s_wmux);
    if (send) post_weight(grams, net);
}

void appliance_scale_stopped(void)
{
    portENTER_CRITICAL(&s_wmux);
    s_have_weight = false;
    s_tare = 0;
    portEXIT_CRITICAL(&s_wmux);
}

esp_err_t appliance_scale_watch(uint32_t seconds)
{
    if (!s_drv || !s_drv->caps->has_scale || !s_drv->scale_watch) return ESP_ERR_NOT_SUPPORTED;
    appliance_status_t st;
    appliance_get_status(&st);
    if (!st.connected) return ESP_ERR_INVALID_STATE;
    return s_drv->scale_watch(seconds);
}

esp_err_t appliance_scale_tare(float *tare_out)
{
    portENTER_CRITICAL(&s_wmux);
    bool have = s_have_weight;
    float gross = s_gross;
    if (have) {
        s_tare = gross;
        s_sent_net = 0;
        s_sent_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_wmux);
    if (!have) return ESP_ERR_INVALID_STATE;
    if (tare_out) *tare_out = gross;
    post_weight(gross, 0);  // sofort 0 zeigen, nicht auf den nächsten Messwert warten
    return ESP_OK;
}

void appliance_report_onboard(appliance_onboard_t state)
{
    esp_event_post(APPLIANCE_EVENT, APPLIANCE_EVENT_ONBOARD, &state, sizeof(state), 0);
}

esp_err_t appliance_send(const appliance_program_t *p)
{
    if (!s_drv) return ESP_ERR_INVALID_STATE;
    const appliance_caps_t *c = s_drv->caps;
    bool temp_ok = p->temperature == 0 || (p->temperature >= c->temp_min && p->temperature <= c->temp_max);
    bool speed_ok = false;
    for (int i = 0; i < c->n_speeds; i++) speed_ok |= c->speeds[i].v == p->speed;
    if (!temp_ok || !speed_ok || p->time_s > c->time_max_s) {
        ESP_LOGW(TAG, "Werte ausserhalb der Grenzen: %d C, %lu s, Stufe %u", p->temperature,
                 (unsigned long)p->time_s, p->speed);
        return ESP_ERR_INVALID_ARG;
    }
    appliance_status_t st;
    appliance_get_status(&st);
    if (!st.connected) return ESP_ERR_INVALID_STATE;
    return s_drv->send_program(p);
}

esp_err_t appliance_stop(void)
{
    return s_drv ? s_drv->stop() : ESP_ERR_INVALID_STATE;
}

esp_err_t appliance_onboard(void)
{
    return s_drv ? s_drv->onboard() : ESP_ERR_INVALID_STATE;
}
