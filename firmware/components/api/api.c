#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "appliance.h"
#include "auth.h"
#include "net.h"
#include "storage.h"

static const char *TAG = "api";

#define MAX_BODY    1024
#define MAX_CLIENTS 8

static httpd_handle_t s_server;

// ---------------------------------------------------------------------------
// Hilfen

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, status, o);
}

static bool authorized(httpd_req_t *req)
{
    char hdr[48];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && auth_check(hdr)) return true;
    send_error(req, "401 Unauthorized", "token");
    return false;
}

// Body als JSON lesen (NULL bei Fehler – Antwort ist dann schon gesendet).
static cJSON *read_body(httpd_req_t *req)
{
    if (req->content_len > MAX_BODY) {
        send_error(req, "413 Payload Too Large", "body");
        return NULL;
    }
    char buf[MAX_BODY + 1];
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            send_error(req, "400 Bad Request", "body");
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) send_error(req, "400 Bad Request", "json");
    return j;
}

static int json_int(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

// Geräteneutraler Status, Feldnamen siehe docs/api.md
static cJSON *status_json(void)
{
    appliance_status_t st;
    appliance_get_status(&st);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "connected", st.connected);
    cJSON_AddBoolToObject(d, "running", st.running);
    cJSON_AddBoolToObject(d, "heating", st.heating);
    cJSON_AddBoolToObject(d, "hot", st.hot);
    cJSON_AddBoolToObject(d, "bowlFitted", st.bowl_fitted);
    cJSON_AddBoolToObject(d, "lidClosed", st.lid_closed);
    cJSON_AddNumberToObject(d, "bowlTemp", st.bowl_temp);
    cJSON_AddNumberToObject(d, "tempSet", st.temp_set);
    cJSON_AddNumberToObject(d, "speedSet", st.speed_set);
    cJSON_AddNumberToObject(d, "remainingS", st.remaining_s);
    return d;
}

// ---------------------------------------------------------------------------
// WebSocket: Ereignisse an alle verbundenen Apps

static void broadcast_work(void *arg)
{
    char *msg = arg;
    size_t n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_server, &n, fds) == ESP_OK) {
        httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg)};
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_server, fds[i], &f);
            }
        }
    }
    free(msg);
}

static void broadcast(const char *type, cJSON *data)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", type);
    cJSON_AddItemToObject(m, "d", data);
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (s && httpd_queue_work(s_server, broadcast_work, s) != ESP_OK) free(s);
}

static void on_appliance_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case APPLIANCE_EVENT_STATUS:
        broadcast("status", status_json());
        break;
    case APPLIANCE_EVENT_ACK: {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "cmd", "setProgramme");
        cJSON_AddBoolToObject(d, "ok", *(bool *)data);
        broadcast("ack", d);
        break;
    }
    case APPLIANCE_EVENT_WEIGHT: {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "g", *(float *)data);
        broadcast("weight", d);
        break;
    }
    case APPLIANCE_EVENT_ONBOARD: {
        static const char *names[] = {"searching", "sent", "done", "failed"};
        appliance_onboard_t st = *(appliance_onboard_t *)data;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "state", st <= APPLIANCE_ONBOARD_FAILED ? names[st] : "failed");
        cJSON_AddNullToObject(d, "reason");
        broadcast("onboard", d);
        break;
    }
    default:
        break;
    }
}

// Token als ?token=… schon beim Handshake prüfen
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    char q[64], tok[40];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "token", tok, sizeof(tok)) == ESP_OK && auth_check(tok)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WebSocket ohne gueltigen Token abgelehnt");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "token");
    return ESP_FAIL;
}

// Nachrichten der App über WS werden (noch) nicht gebraucht – Befehle laufen über HTTP.
static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len == 0) return err;
    uint8_t buf[128];
    f.payload = buf;
    return httpd_ws_recv_frame(req, &f, f.len < sizeof(buf) ? f.len : sizeof(buf));
}

// ---------------------------------------------------------------------------
// HTTP

static esp_err_t h_info(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", net_device_id());
    cJSON_AddStringToObject(o, "name", "Freikoch Brücke");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "api", 1);

    uint64_t total = 0, free_b = 0;
    cJSON *s = cJSON_AddObjectToObject(o, "storage");
    cJSON_AddStringToObject(s, "backend", storage_backend());
    if (storage_usage(&total, &free_b) == ESP_OK) {
        cJSON_AddNumberToObject(s, "totalKB", (double)(total / 1024));
        cJSON_AddNumberToObject(s, "freeKB", (double)(free_b / 1024));
    }

    appliance_status_t st;
    appliance_get_status(&st);
    const appliance_caps_t *c = appliance_caps();
    cJSON *a = cJSON_AddObjectToObject(o, "appliance");
    cJSON_AddBoolToObject(a, "paired", st.connected);  // TODO: dauerhaft merken, sobald es die Kopplung gibt
    cJSON_AddBoolToObject(a, "connected", st.connected);
    if (c) {
        cJSON *k = cJSON_AddObjectToObject(a, "caps");
        cJSON_AddStringToObject(k, "model", c->model);
        cJSON_AddStringToObject(k, "name", c->name);
        cJSON_AddNumberToObject(k, "tempMin", c->temp_min);
        cJSON_AddNumberToObject(k, "tempMax", c->temp_max);
        static const char *groups[] = {"off", "stir", "continuous"};
        cJSON *sp = cJSON_AddArrayToObject(k, "speeds");
        for (int i = 0; i < c->n_speeds; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "v", c->speeds[i].v);
            cJSON_AddStringToObject(e, "label", c->speeds[i].label);
            cJSON_AddStringToObject(e, "group", groups[c->speeds[i].group]);
            cJSON_AddItemToArray(sp, e);
        }
        cJSON_AddNumberToObject(k, "timeMaxS", c->time_max_s);
        cJSON_AddBoolToObject(k, "hasScale", c->has_scale);
        cJSON_AddBoolToObject(k, "remoteStart", c->remote_start);
    }
    return send_json(req, "200 OK", o);
}

static esp_err_t h_state(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    return send_json(req, "200 OK", status_json());
}

static esp_err_t h_program(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *b = read_body(req);
    if (!b) return ESP_OK;
    int temp = json_int(b, "temperature", 0), time_s = json_int(b, "time", 0), speed = json_int(b, "speed", 0);
    cJSON_Delete(b);
    if (temp < 0 || time_s < 0 || speed < 0 || speed > 255) return send_error(req, "400 Bad Request", "range");
    const appliance_program_t p = {.temperature = temp, .time_s = time_s, .speed = speed};
    esp_err_t err = appliance_send(&p);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "range");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "not_connected");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

static esp_err_t h_stop(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    esp_err_t err = appliance_stop();
    if (err != ESP_OK) return send_error(req, "409 Conflict", "not_connected");
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

static esp_err_t h_onboard(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    esp_err_t err = appliance_onboard();
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "busy");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

static esp_err_t h_time(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *b = read_body(req);
    if (!b) return ESP_OK;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(b, "epoch");
    if (cJSON_IsNumber(e) && e->valuedouble > 1700000000) {
        struct timeval tv = {.tv_sec = (time_t)e->valuedouble};
        settimeofday(&tv, NULL);
    }
    cJSON_Delete(b);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static esp_err_t h_pair(httpd_req_t *req)
{
    cJSON *b = req->content_len ? read_body(req) : cJSON_CreateObject();
    if (!b) return ESP_OK;
    char id[24], token[40];
    auth_pair_result_t r = auth_pair(json_str(b, "id"), json_str(b, "name"), id, sizeof(id), token, sizeof(token));
    cJSON_Delete(b);
    cJSON *o = cJSON_CreateObject();
    if (r == AUTH_PAIR_TOKEN) {
        cJSON_AddStringToObject(o, "token", token);
        return send_json(req, "200 OK", o);
    }
    cJSON_AddStringToObject(o, "pairing", "waiting");
    cJSON_AddStringToObject(o, "id", id);
    return send_json(req, "202 Accepted", o);
}

static esp_err_t h_pair_approve(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *b = read_body(req);
    if (!b) return ESP_OK;
    bool ok = auth_approve(json_str(b, "id"));
    cJSON_Delete(b);
    if (!ok) return send_error(req, "404 Not Found", "request");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static void on_pair_request(const char *id, const char *name)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "id", id);
    cJSON_AddStringToObject(d, "name", name);
    broadcast("pairRequest", d);
}

esp_err_t api_start(void)
{
    auth_init(on_pair_request);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = MAX_CLIENTS - 1;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 6144;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Server: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/api/v1/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/v1/state", .method = HTTP_GET, .handler = h_state},
        {.uri = "/api/v1/machine/program", .method = HTTP_POST, .handler = h_program},
        {.uri = "/api/v1/machine/stop", .method = HTTP_POST, .handler = h_stop},
        {.uri = "/api/v1/machine/onboard", .method = HTTP_POST, .handler = h_onboard},
        {.uri = "/api/v1/time", .method = HTTP_POST, .handler = h_time},
        {.uri = "/api/v1/pair", .method = HTTP_POST, .handler = h_pair},
        {.uri = "/api/v1/pair/approve", .method = HTTP_POST, .handler = h_pair_approve},
        {.uri = "/api/v1/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
         .ws_pre_handshake_cb = ws_pre_handshake},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);

    esp_event_handler_register(APPLIANCE_EVENT, ESP_EVENT_ANY_ID, on_appliance_event, NULL);
    ESP_LOGI(TAG, "App-API auf Port 80 bereit");
    return ESP_OK;
}
