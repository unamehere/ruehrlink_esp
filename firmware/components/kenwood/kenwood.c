#include "kenwood.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net.h"
#include "led.h"
#include "setup_cert.h"

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
extern const uint8_t setup_cert_start[] asm("_binary_setup_server_pem_start");
extern const uint8_t setup_cert_end[] asm("_binary_setup_server_pem_end");

// Stufen in der Reihenfolge des Drehknopfs: Rührintervalle/Puls ← Aus → Dauerstufen.
// `v` = Wert, der an die Maschine geht; Labels wie am Display.
static const appliance_speed_t s_speeds[] = {
#if CONFIG_FK_SPEED_TEST
    // Testmodus: rohe Werte, um die Zuordnung am Display abzulesen (docs/testplan.md, Abschnitt 1)
    {0, "0", APPLIANCE_SPEED_OFF}, {1, "1", APPLIANCE_SPEED_CONTINUOUS}, {2, "2", APPLIANCE_SPEED_CONTINUOUS},
    {3, "3", APPLIANCE_SPEED_CONTINUOUS}, {4, "4", APPLIANCE_SPEED_CONTINUOUS}, {5, "5", APPLIANCE_SPEED_CONTINUOUS},
    {6, "6", APPLIANCE_SPEED_CONTINUOUS}, {7, "7", APPLIANCE_SPEED_CONTINUOUS}, {8, "8", APPLIANCE_SPEED_CONTINUOUS},
    {9, "9", APPLIANCE_SPEED_CONTINUOUS}, {10, "10", APPLIANCE_SPEED_CONTINUOUS}, {11, "11", APPLIANCE_SPEED_CONTINUOUS},
    {12, "12", APPLIANCE_SPEED_CONTINUOUS}, {13, "13", APPLIANCE_SPEED_CONTINUOUS}, {14, "14", APPLIANCE_SPEED_CONTINUOUS},
    {15, "15", APPLIANCE_SPEED_CONTINUOUS},
#else
    // Ermittelt an der KCL95 am 12.11.2026 (docs/testplan.md). Werte liegen NICHT in Knopf-Reihenfolge.
    // P (Puls) ist per Fernsteuerung nicht verfügbar – nur am Gerät.
    {3, "4", APPLIANCE_SPEED_STIR},
    {2, "3", APPLIANCE_SPEED_STIR},
    {1, "2", APPLIANCE_SPEED_STIR},
    {4, "1", APPLIANCE_SPEED_STIR},
    {0, "Aus", APPLIANCE_SPEED_OFF},
    {5, "min", APPLIANCE_SPEED_CONTINUOUS},
    {6, "1", APPLIANCE_SPEED_CONTINUOUS},
    {7, "2", APPLIANCE_SPEED_CONTINUOUS},
    {8, "3", APPLIANCE_SPEED_CONTINUOUS},
    {9, "4", APPLIANCE_SPEED_CONTINUOUS},
    {10, "5", APPLIANCE_SPEED_CONTINUOUS},
    {11, "6", APPLIANCE_SPEED_CONTINUOUS},
    {12, "max", APPLIANCE_SPEED_CONTINUOUS},
#endif
};

// Werte vorläufig, an der Maschine zu prüfen (siehe docs/testplan.md)
static const appliance_caps_t s_caps = {
    .model = "kenwood-kcl95",
    .name = "Cooking Chef XL",
    .temp_min = 20,
    .temp_max = 180,
    .speeds = s_speeds,
    .n_speeds = sizeof(s_speeds) / sizeof(s_speeds[0]),
    .time_max_s = 8 * 3600,
    .has_scale = true,
    .remote_start = false,
};

// Alles Folgende wird nur im Task des HTTP-Servers verändert (Handler und httpd_queue_work).
static httpd_handle_t s_server;
static int s_ws_fd = -1;
static int64_t s_last_rx_us;
// HeadLiftInterlockClosed steht nicht im vollen Status der KCL95, sondern kommt (wenn überhaupt) nur als
// Änderung. Ohne Meldung gilt der Kopf als geschlossen – sonst zeigt die App dauerhaft „Kopf schließen“.
// Die Maschine verriegelt den Motor bei offenem Kopf ohnehin selbst.
static appliance_status_t s_st = { .lid_closed = true };
static esp_timer_handle_t s_ping_timer;
static esp_timer_handle_t s_ack_timer;
static volatile uint32_t s_connect_count;   // zählt neue Verbindungen (für die Kopplung)
static volatile bool s_onboarding;

// ---------------------------------------------------------------------------
// Verbindungszustand

static void set_connected(bool on)
{
    if (s_st.connected == on) return;
    s_st.connected = on;
    if (!on) s_ws_fd = -1;
    else s_connect_count++;
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

// ---------------------------------------------------------------------------
// Kopplung: Maschine im Modus "WiFi -> Verbinden" öffnet ein offenes Setup-WLAN "KENWOOD…"
// (IP 192.168.4.1). Wir verbinden uns damit und übergeben per HTTPS POST /join unser
// Maschinen-WLAN. Siehe docs/architektur.md.

#define SETUP_PREFIX   "KENWOOD"
#define SETUP_URL      "https://192.168.4.1"
#define HOLD_CHANNEL_S 180

// Pinning: Der Einrichtungs-Server der Maschine nutzt ein selbstsigniertes, seit 2021 abgelaufenes
// Zertifikat (Drop Kitchen, CN 192.168.4.1, certs/setup_server.pem). Wir vertrauen genau diesem
// einen Zertifikat, Byte für Byte; nur die Zeitprüfung entfällt (abgelaufen, Brücke ohne Uhr).
static mbedtls_x509_crt s_pin;
static bool s_pin_loaded;

static int pin_verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    bool same = crt->raw.len == s_pin.raw.len && memcmp(crt->raw.p, s_pin.raw.p, crt->raw.len) == 0;
    if (same) *flags = 0;
    else *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    return 0;
}

static esp_err_t pin_attach(void *conf)
{
    if (!s_pin_loaded) {
        mbedtls_x509_crt_init(&s_pin);
        if (mbedtls_x509_crt_parse(&s_pin, setup_cert_start, setup_cert_end - setup_cert_start) != 0) {
            return ESP_FAIL;
        }
        s_pin_loaded = true;
    }
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(conf, &s_pin, NULL);
    mbedtls_ssl_conf_verify(conf, pin_verify, NULL);
    return ESP_OK;
}

static esp_err_t https_setup(const char *path, esp_http_client_method_t method, const char *body)
{
    char url[48];
    snprintf(url, sizeof(url), SETUP_URL "%s", path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 12000,
        .crt_bundle_attach = pin_attach,  // nur das hinterlegte Zertifikat der Maschine
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    esp_err_t err = esp_http_client_perform(c);
    ESP_LOGI(TAG, "Kopplung %s: %s, HTTP %d", path, esp_err_to_name(err), esp_http_client_get_status_code(c));
    esp_http_client_cleanup(c);
    return err;
}

static void onboard_task(void *arg)
{
    char ssid[33] = "";
    vTaskDelay(pdMS_TO_TICKS(1000));  // 202 an die App rausgehen lassen, bevor das Heim-WLAN getrennt wird
    appliance_report_onboard(APPLIANCE_ONBOARD_SEARCHING);
    led_signal(LED_SETUP);
    net_ap_use_default();  // Maschine immer auf das reguläre "Freikoch-XXXX" koppeln

    esp_err_t err = net_temp_join(SETUP_PREFIX, ssid, sizeof(ssid), 20);
    if (err == ESP_OK) {
        err = https_setup("/mac", HTTP_METHOD_GET, NULL);  // wie die Original-App; prüft zugleich das Zertifikat
        if (err != ESP_OK) {
#if CONFIG_FK_DEVELOPER_MODE
            // Diagnose: anderes Zertifikat? Nur ins serielle Log, wird NICHT übernommen.
            setup_cert_capture("192.168.4.1");
#endif
        } else {
            char body[160];
            snprintf(body, sizeof(body), "{\"ssid\":\"%s\",\"password\":\"%s\"}", net_ap_ssid(), net_ap_password());
            err = https_setup("/join", HTTP_METHOD_POST, body);
        }
    }
    net_temp_leave(err == ESP_OK ? HOLD_CHANNEL_S : 0);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Kopplung fehlgeschlagen (%s)", esp_err_to_name(err));
        appliance_report_onboard(APPLIANCE_ONBOARD_FAILED);
        led_signal(LED_ERROR);
    } else {
        appliance_report_onboard(APPLIANCE_ONBOARD_SENT);
        uint32_t before = s_connect_count;
        for (int i = 0; i < HOLD_CHANNEL_S && s_connect_count == before; i++) vTaskDelay(pdMS_TO_TICKS(1000));
        bool ok = s_connect_count != before;
        net_home_pause(0);
        ESP_LOGI(TAG, "Kopplung %s", ok ? "erfolgreich" : "ohne Verbindung der Maschine beendet");
        appliance_report_onboard(ok ? APPLIANCE_ONBOARD_DONE : APPLIANCE_ONBOARD_FAILED);
        led_signal(ok ? LED_OK : LED_ERROR);
    }
    s_onboarding = false;
    vTaskDelete(NULL);
}

static esp_err_t onboard(void)
{
    if (s_onboarding) return ESP_ERR_INVALID_STATE;
    s_onboarding = true;
    if (xTaskCreate(onboard_task, "kw_onboard", 6144, NULL, 4, NULL) != pdPASS) {
        s_onboarding = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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
        s_st.lid_closed = true;  // unbekannt bis zur nächsten Meldung (siehe s_st)
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
