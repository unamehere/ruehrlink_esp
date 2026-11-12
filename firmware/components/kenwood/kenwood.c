#include "kenwood.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "kenwood";

// Protokoll siehe docs/architektur.md – verifiziert mit dem MicroPython-Prototyp an einer KCL95.
#define PING_INTERVAL_US  (20 * 1000000LL)   // ohne Ping trennt die Maschine nach ~60 s
#define RX_TIMEOUT_US     (70 * 1000000LL)   // so lange nichts gehört → Verbindung gilt als tot
#define ACK_TIMEOUT_US    (5 * 1000000LL)
#define MAX_FRAME_LEN     4096

extern const uint8_t cert_start[] asm("_binary_machine_cert_pem_start");
extern const uint8_t cert_end[] asm("_binary_machine_cert_pem_end");
extern const uint8_t key_start[] asm("_binary_machine_key_pem_start");
extern const uint8_t key_end[] asm("_binary_machine_key_pem_end");

// Werte vorläufig, an der Maschine zu prüfen (siehe docs/testplan.md)
static const appliance_caps_t s_caps = {
    .model = "kenwood-kcl95",
    .name = "Cooking Chef XL",
    .temp_min = 20,
    .temp_max = 180,
    .speed_max = 10,
    .time_max_s = 8 * 3600,
    .has_scale = true,
    .remote_start = false,
};

// Alles Folgende wird nur im Task des HTTP-Servers verändert (Handler und httpd_queue_work).
static httpd_handle_t s_server;
static int s_ws_fd = -1;
static int64_t s_last_rx_us;
static appliance_status_t s_st;
static esp_timer_handle_t s_ping_timer;
static esp_timer_handle_t s_ack_timer;

// ---------------------------------------------------------------------------
// Verbindungszustand

static void set_connected(bool on)
{
    if (s_st.connected == on) return;
    s_st.connected = on;
    if (!on) s_ws_fd = -1;
    ESP_LOGI(TAG, "%s", on ? "*** Maschine verbunden ***" : "Maschine getrennt");
    appliance_set_status(&s_st);
}

static void on_session_close(httpd_handle_t hd, int fd)
{
    if (fd == s_ws_fd) set_connected(false);
    close(fd);
}

// ---------------------------------------------------------------------------
// Senden (aus beliebigem Task → über httpd_queue_work in den Server-Task)

static void send_text_work(void *arg)
{
    char *json = arg;
    if (s_ws_fd >= 0) {
        httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = strlen(json)};
        if (httpd_ws_send_frame_async(s_server, s_ws_fd, &f) == ESP_OK) {
            ESP_LOGI(TAG, "-> %s", json);
        } else {
            ESP_LOGW(TAG, "Senden fehlgeschlagen");
            httpd_sess_trigger_close(s_server, s_ws_fd);
        }
    }
    free(json);
}

static void ping_work(void *arg)
{
    if (s_ws_fd < 0) return;
    if (esp_timer_get_time() - s_last_rx_us > RX_TIMEOUT_US) {
        ESP_LOGW(TAG, "keine Antwort der Maschine seit %lld s", RX_TIMEOUT_US / 1000000);
        httpd_sess_trigger_close(s_server, s_ws_fd);
        return;
    }
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_PING};
    if (httpd_ws_send_frame_async(s_server, s_ws_fd, &f) != ESP_OK) {
        httpd_sess_trigger_close(s_server, s_ws_fd);
    }
}

static void ping_timer_cb(void *arg) { httpd_queue_work(s_server, ping_work, NULL); }

static void ack_timeout_cb(void *arg)
{
    ESP_LOGW(TAG, "keine Bestaetigung der Maschine");
    appliance_report_ack(false);
}

static esp_err_t send_program(const appliance_program_t *p)
{
    char *json = malloc(128);
    if (!json) return ESP_ERR_NO_MEM;
    // Temperatur als Kommazahl, wie die Original-Cloud
    snprintf(json, 128, "{\"command\":\"setProgramme\",\"settings\":{\"time\":%lu,\"temperature\":%d.0,\"speed\":%u}}",
             (unsigned long)p->time_s, p->temperature, p->speed);
    esp_err_t err = httpd_queue_work(s_server, send_text_work, json);
    if (err != ESP_OK) {
        free(json);
        return err;
    }
    esp_timer_stop(s_ack_timer);
    esp_timer_start_once(s_ack_timer, ACK_TIMEOUT_US);
    return ESP_OK;
}

// setProgramme 0/0/0 stoppt NICHT; Zeit 1 s lässt das Programm sofort auslaufen.
static esp_err_t stop(void)
{
    const appliance_program_t p = {.temperature = 0, .time_s = 1, .speed = 0};
    return send_program(&p);
}

static esp_err_t onboard(void)
{
    // TODO: Setup-WLAN "KENWOOD…" suchen, POST https://192.168.4.1/join (siehe architektur.md)
    return ESP_ERR_NOT_SUPPORTED;
}

// ---------------------------------------------------------------------------
// Empfang

static void read_bool(const cJSON *d, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, key);
    if (cJSON_IsBool(v)) *out = cJSON_IsTrue(v);
}

static void read_num(const cJSON *d, const char *key, int32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, key);
    if (cJSON_IsNumber(v)) *out = (int32_t)v->valuedouble;
}

static void handle_status(const cJSON *d, const char *raw)
{
    int32_t bowl = s_st.bowl_temp, tset = s_st.temp_set, speed = s_st.speed_set, rem = s_st.remaining_s;
    read_num(d, "bowlTemperature", &bowl);
    read_num(d, "temperatureSetting", &tset);
    read_num(d, "speedSetting", &speed);
    read_num(d, "remainingTime", &rem);
    s_st.bowl_temp = bowl;
    s_st.temp_set = tset;
    s_st.speed_set = speed;
    s_st.remaining_s = rem < 0 ? 0 : rem;
    read_bool(d, "motorOn", &s_st.running);
    read_bool(d, "heating", &s_st.heating);
    read_bool(d, "hot", &s_st.hot);
    read_bool(d, "bowlFitted", &s_st.bowl_fitted);
    read_bool(d, "HeadLiftInterlockClosed", &s_st.lid_closed);

    // reine Temperatur-Schwankungen (~1/s) nicht protokollieren
    bool only_temps = true;
    for (const cJSON *c = d->child; c; c = c->next) {
        if (strcmp(c->string, "bowlTemperature") && strcmp(c->string, "heaterTemperature") &&
            strcmp(c->string, "motorTemperature")) {
            only_temps = false;
            break;
        }
    }
    if (!only_temps) ESP_LOGI(TAG, "Status: %.300s", raw);
    appliance_set_status(&s_st);
}

static void handle_message(char *raw)
{
    cJSON *m = cJSON_Parse(raw);
    if (!m) {
        ESP_LOGW(TAG, "<- kein JSON: %.120s", raw);
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(m, "type");
    const cJSON *ack = cJSON_GetObjectItemCaseSensitive(m, "acknowledged");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "status") == 0) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "data");
        if (cJSON_IsObject(d)) handle_status(d, raw);
    } else if (cJSON_IsString(ack)) {
        bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "result"));
        esp_timer_stop(s_ack_timer);
        ESP_LOGI(TAG, "ACK %s: %s", ack->valuestring, ok ? "ok" : "abgelehnt");
        appliance_report_ack(ok);
    } else {
        ESP_LOGI(TAG, "<- %.200s", raw);
    }
    cJSON_Delete(m);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    // ESP-IDF 6 ruft den Handler beim Handshake nicht auf. Die Maschine schickt direkt nach
    // dem Verbinden ihren Status – der erste Frame einer neuen Verbindung übernimmt sie.
    int fd = httpd_req_to_sockfd(req);
    if (fd != s_ws_fd) {
        if (s_ws_fd >= 0) httpd_sess_trigger_close(s_server, s_ws_fd);
        s_st.connected = false;  // damit set_connected() das Ereignis sicher auslöst
        s_ws_fd = fd;
        set_connected(true);
    }

    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);  // nur Länge
    if (err != ESP_OK) return err;
    if (f.len > MAX_FRAME_LEN) return ESP_FAIL;
    uint8_t *buf = calloc(1, f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    if (f.len && (err = httpd_ws_recv_frame(req, &f, f.len)) != ESP_OK) {
        free(buf);
        return err;
    }
    s_last_rx_us = esp_timer_get_time();

    switch (f.type) {
    case HTTPD_WS_TYPE_TEXT:
        handle_message((char *)buf);
        break;
    case HTTPD_WS_TYPE_PING:
        f.type = HTTPD_WS_TYPE_PONG;
        httpd_ws_send_frame(req, &f);
        break;
    case HTTPD_WS_TYPE_CLOSE:
        ESP_LOGI(TAG, "Maschine schliesst WebSocket");
        f.len = 0;
        httpd_ws_send_frame(req, &f);
        set_connected(false);
        break;
    default:  // PONG: nur Lebenszeichen
        break;
    }
    free(buf);
    return ESP_OK;
}

// Boot-Abfragen der Maschine. Firmware-Updates werden immer abgelehnt (Produkt bleibt kompatibel).
static esp_err_t devices_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %.100s", req->uri);
    httpd_resp_set_type(req, "application/json");
    if (strncmp(req->uri, "/devices/hello", 14) == 0) {
        return httpd_resp_sendstr(req,
            "{\"action\":\"connect\",\"data\":{\"url\":\"wss://events-shards1.getdrop.io/ws/device/local\"}}");
    }
    return httpd_resp_sendstr(req, "{\"action\":\"none\"}");
}

static esp_err_t fallback_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "unbekannte Anfrage: %.100s", req->uri);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"action\":\"none\"}");
}

// ---------------------------------------------------------------------------

static esp_err_t start(void)
{
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert = cert_start;
    cfg.servercert_len = cert_end - cert_start;
    cfg.prvtkey_pem = key_start;
    cfg.prvtkey_len = key_end - key_start;
    cfg.httpd.max_open_sockets = 3;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.httpd.close_fn = on_session_close;
    cfg.httpd.ctrl_port = 32770;  // App-API nutzt den Standard

    esp_err_t err = httpd_ssl_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TLS-Server: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/devices/*", .method = HTTP_GET, .handler = devices_handler},
        {.uri = "/ws/*", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
         .handle_ws_control_frames = true},
        {.uri = "/*", .method = HTTP_GET, .handler = fallback_handler},
        {.uri = "/*", .method = HTTP_POST, .handler = fallback_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);

    const esp_timer_create_args_t ping = {.callback = ping_timer_cb, .name = "kw_ping"};
    const esp_timer_create_args_t ack = {.callback = ack_timeout_cb, .name = "kw_ack"};
    esp_timer_create(&ping, &s_ping_timer);
    esp_timer_create(&ack, &s_ack_timer);
    esp_timer_start_periodic(s_ping_timer, PING_INTERVAL_US);

    ESP_LOGI(TAG, "TLS-Server auf Port 443 bereit");
    return ESP_OK;
}

static const appliance_driver_t s_driver = {
    .caps = &s_caps,
    .start = start,
    .send_program = send_program,
    .stop = stop,
    .onboard = onboard,
};

const appliance_driver_t *kenwood_driver(void) { return &s_driver; }
