#include "appliance.h"

#include <string.h>

#include "esp_log.h"
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

void appliance_report_weight(float grams)
{
    esp_event_post(APPLIANCE_EVENT, APPLIANCE_EVENT_WEIGHT, &grams, sizeof(grams), 0);
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
    if (!temp_ok || p->speed > c->speed_max || p->time_s > c->time_max_s) {
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
