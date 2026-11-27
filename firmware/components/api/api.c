#include "api.h"

#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "appliance.h"
#include "auth.h"
#include "diag.h"
#include "factory.h"
#include "net.h"
#include "recipes.h"
#include "sequencer.h"
#include "storage.h"
#include "update.h"

static const char *TAG = "api";

#define MAX_BODY    4096   // Programme mit bis zu 20 Schritten
#define MAX_CLIENTS 8

// App-API (docs/api.md): innerhalb einer Hauptversion nur additive Änderungen.
// Bruch → API_VERSION + 1 und die alte Version noch mindestens ein Jahr in API_SUPPORTED mitführen.
#define API_VERSION 1
static const int API_SUPPORTED[] = {1};

// Was diese Firmware kann; die App schaltet Funktionen danach frei. Einträge nur hinzufügen,
// innerhalb einer API_VERSION nie entfernen oder umbenennen.
static const char *const FEATURES[] = {
    "machine",          // POST /machine/program, /machine/stop
    "machine.onboard",  // POST /machine/onboard
    "machine.repair",   // POST /machine/repair (Maschine neu koppeln, ohne Werksreset)
    "scale",            // POST /scale/watch, WS weight
    "scale.tare",       // POST /scale/tare
    "sequence",         // /sequence/start|abort, GET /sequence, WS seq
    "programs",         // GET /programs
    "recipes",          // /recipes, WS recipes
    "media",            // /media
    "time",             // POST /time
    "pair",             // POST /pair, /pair/approve, WS pairRequest
    "reset",            // POST /reset
    "reset.wipe",       // POST /reset {"wipe":true}: zusätzlich Rezepte, Fotos, Protokolle löschen
    "ota",              // POST /ota
    "diag",             // GET /diag
};

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
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && auth_check(hdr)) {
        update_confirm();  // erste erfolgreiche Anfrage nach einem Update → neue Firmware ist gut
        return true;
    }
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
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return NULL;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            free(buf);
            send_error(req, "400 Bad Request", "body");
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) send_error(req, "400 Bad Request", "json");
    return j;
}

static int json_int(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// Geräteneutraler Status, Feldnamen siehe docs/api.md
static cJSON *status_json(void)
{
    appliance_status_t st;
    appliance_get_status(&st);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "connected", st.connected);
    cJSON_AddBoolToObject(d, "running", st.running);
    cJSON_AddBoolToObject(d, "active", st.active);
    cJSON_AddBoolToObject(d, "keepWarm", st.keep_warm);
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

// Zustand des laufenden Programms, Feldnamen siehe docs/api.md (WS "seq")
static cJSON *seq_json(const sequencer_info_t *i)
{
    cJSON *d = cJSON_CreateObject();
    if (i->program_id[0]) cJSON_AddStringToObject(d, "programId", i->program_id);
    else cJSON_AddNullToObject(d, "programId");
    cJSON_AddStringToObject(d, "name", i->name);
    cJSON_AddNumberToObject(d, "step", i->step);
    cJSON_AddNumberToObject(d, "of", i->n_steps);
    cJSON_AddStringToObject(d, "label", i->label);
    cJSON_AddStringToObject(d, "state", sequencer_state_name(i->state));
    const char *r = sequencer_reason_name(i->reason);
    if (r) cJSON_AddStringToObject(d, "reason", r);
    else cJSON_AddNullToObject(d, "reason");
    cJSON_AddNumberToObject(d, "stepRemainingS", i->step_remaining_s);
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
    if (!s && strcmp(type, "weight") != 0) ESP_LOGW(TAG, "WS: %s nicht verteilt (kein RAM)", type);
    if (s && httpd_queue_work(s_server, broadcast_work, s) != ESP_OK) {
        free(s);
        // Waage-Nachrichten kommen oft – Verluste dort nur selten melden, alles andere immer (F26)
        static uint32_t dropped;
        if (strcmp(type, "weight") != 0 || (dropped++ % 50) == 0) ESP_LOGW(TAG, "WS: %s nicht verteilt", type);
    }
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
        const appliance_weight_t *w = data;
        cJSON *d = cJSON_CreateObject();
        // auf 0,1 g runden – mehr kann die Waage nicht, und die Nachricht bleibt kurz
        cJSON_AddNumberToObject(d, "g", roundf(w->net * 10) / 10);
        cJSON_AddNumberToObject(d, "gross", roundf(w->gross * 10) / 10);
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

static void on_sequencer_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    broadcast("seq", seq_json((const sequencer_info_t *)data));
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
    cJSON_AddStringToObject(o, "name", "Rührlink");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "api", API_VERSION);
    cJSON *sup = cJSON_AddArrayToObject(o, "apiSupported");
    for (size_t i = 0; i < sizeof(API_SUPPORTED) / sizeof(API_SUPPORTED[0]); i++) {
        cJSON_AddItemToArray(sup, cJSON_CreateNumber(API_SUPPORTED[i]));
    }
    cJSON *feat = cJSON_AddArrayToObject(o, "features");
    for (size_t i = 0; i < sizeof(FEATURES) / sizeof(FEATURES[0]); i++) {
        cJSON_AddItemToArray(feat, cJSON_CreateString(FEATURES[i]));
    }

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
    cJSON_AddBoolToObject(a, "paired", appliance_paired() || st.connected);
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
        cJSON_AddNumberToObject(k, "hotTemp", c->hot_temp);
        cJSON_AddNumberToObject(k, "hotSpeedMax", c->hot_speed_max);
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
    if (sequencer_active()) return send_error(req, "409 Conflict", "busy");
    const appliance_program_t p = {.temperature = temp, .time_s = time_s, .speed = speed};
    esp_err_t err = appliance_send(&p);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "range");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "not_connected");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

// Waage: Die App meldet alle ~10 s, dass sie die Waage anzeigt (Lease); ohne Meldung schaltet die Brücke
// sie nach Ablauf ab (eigene TLS-Verbindung zur Maschine kostet RAM).
#define SCALE_WATCH_DEFAULT_S 30
#define SCALE_WATCH_MAX_S     300

static esp_err_t h_scale_watch(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    uint32_t s = SCALE_WATCH_DEFAULT_S;
    char buf[64];
    int n = req->content_len > 0 && req->content_len < sizeof(buf) ? httpd_req_recv(req, buf, req->content_len) : 0;
    if (n > 0) {
        buf[n] = '\0';
        cJSON *j = cJSON_Parse(buf);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "seconds");
        if (cJSON_IsNumber(v) && v->valuedouble >= 1) {
            s = v->valuedouble > SCALE_WATCH_MAX_S ? SCALE_WATCH_MAX_S : (uint32_t)v->valuedouble;
        }
        cJSON_Delete(j);
    }
    esp_err_t err = appliance_scale_watch(s);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, "501 Not Implemented", "no_scale");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "not_connected");
    if (err != ESP_OK) return send_error(req, "503 Service Unavailable", "busy");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seconds", s);
    return send_json(req, "200 OK", o);
}

static esp_err_t h_scale_tare(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    float tare = 0;
    if (appliance_scale_tare(&tare) != ESP_OK) return send_error(req, "409 Conflict", "no_weight");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "g", 0);
    cJSON_AddNumberToObject(o, "tare", roundf(tare * 10) / 10);
    return send_json(req, "200 OK", o);
}

static esp_err_t h_stop(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    // Läuft ein Programm, wird es abgebrochen (das stoppt auch die Maschine).
    esp_err_t err = sequencer_active() ? sequencer_abort() : appliance_stop();
    if (err != ESP_OK) return send_error(req, "409 Conflict", "not_connected");
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

// --- Programme ---------------------------------------------------------------

#define PROGRAM_DIR STORAGE_ROOT "/programs"
#define MAX_PROGRAM 8192

// "steps":[{"speed","time","temperature","label"}] → Schritte; -1 bei falscher Form,
// SEQUENCER_MAX_STEPS + 1 bei zu vielen.
static int parse_steps(const cJSON *arr, sequencer_step_t *out)
{
    if (!cJSON_IsArray(arr)) return -1;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (n >= SEQUENCER_MAX_STEPS) return SEQUENCER_MAX_STEPS + 1;
        int t = json_int(e, "temperature", 0), time_s = json_int(e, "time", 0), sp = json_int(e, "speed", -1);
        if (t < 0 || t > 1000 || time_s < 0 || sp < 0 || sp > 255) return -1;
        out[n] = (sequencer_step_t){.temperature = t, .time_s = time_s, .speed = sp};
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(e, "label");
        if (cJSON_IsString(l)) strlcpy(out[n].label, l->valuestring, sizeof(out[n].label));
        n++;
    }
    return n;
}

// Programm aus /data/programs/<id>.json (nur a-z, 0-9, - und _ im Namen)
static cJSON *load_program(const char *id)
{
    if (!id[0] || strlen(id) > 31 || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(id)) return NULL;
    char path[64];
    snprintf(path, sizeof(path), PROGRAM_DIR "/%s.json", id);
    char *txt = storage_read(path, MAX_PROGRAM, NULL);
    if (!txt) return NULL;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    return j;
}

static esp_err_t h_seq_start(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *b = read_body(req);
    if (!b) return ESP_OK;
    char pid[32];
    strlcpy(pid, json_str(b, "programId"), sizeof(pid));
    cJSON *prog = NULL;
    if (pid[0]) {
        prog = load_program(pid);
        if (!prog) {
            cJSON_Delete(b);
            return send_error(req, "404 Not Found", "program");
        }
    }
    const cJSON *src = prog ? prog : b;
    sequencer_step_t *steps = calloc(SEQUENCER_MAX_STEPS, sizeof(sequencer_step_t));
    int n = steps ? parse_steps(cJSON_GetObjectItemCaseSensitive(src, "steps"), steps) : -1;
    char name[40];
    strlcpy(name, json_str(src, "name"), sizeof(name));
    cJSON_Delete(prog);
    cJSON_Delete(b);
    if (!steps) return httpd_resp_send_500(req);
    if (n < 0) {
        free(steps);
        return send_error(req, "400 Bad Request", "range");
    }
    sequencer_error_t e = {.code = "too_many", .step = -1};
    esp_err_t err = n > SEQUENCER_MAX_STEPS ? ESP_ERR_INVALID_ARG : sequencer_start(pid, name, steps, n, &e);
    free(steps);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", e.code ? e.code : "range");
        if (e.step >= 0) cJSON_AddNumberToObject(o, "step", e.step);
        return send_json(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict", o);
    }
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(20));  // dem Ablauf Zeit geben, Schritt 1 einzutragen
    sequencer_info_t info;
    sequencer_get(&info);
    return send_json(req, "202 Accepted", seq_json(&info));
}

static esp_err_t h_seq_abort(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (sequencer_abort() != ESP_OK) return send_error(req, "409 Conflict", "idle");
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

static esp_err_t h_seq_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    sequencer_info_t info;
    sequencer_get(&info);
    return send_json(req, "200 OK", seq_json(&info));
}

static esp_err_t h_programs(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(o, "programs");
    DIR *d = opendir(PROGRAM_DIR);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        char id[40];
        strlcpy(id, de->d_name, sizeof(id));
        char *dot = strrchr(id, '.');
        if (!dot || strcasecmp(dot, ".json")) continue;
        *dot = '\0';
        for (char *c = id; *c; c++) *c = (char)tolower((unsigned char)*c);  // FAT liefert evtl. Großbuchstaben
        cJSON *p = load_program(id);
        if (!p) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", id);
        cJSON_AddStringToObject(e, "name", json_str(p, "name"));
        cJSON *steps = cJSON_DetachItemFromObjectCaseSensitive(p, "steps");
        cJSON_AddItemToObject(e, "steps", steps ? steps : cJSON_CreateArray());
        cJSON_AddItemToArray(list, e);
        cJSON_Delete(p);
    }
    if (d) closedir(d);
    return send_json(req, "200 OK", o);
}

static esp_err_t h_onboard(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    esp_err_t err = appliance_onboard();
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "busy");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

// Maschine neu koppeln: Kopplung vergessen, Heim-WLAN, App-Kopplungen und Rezepte bleiben.
// Danach führt die App durch die Kopplung (POST /machine/onboard).
static esp_err_t h_repair(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    appliance_status_t st;
    appliance_get_status(&st);
    if (sequencer_active() || st.active) return send_error(req, "409 Conflict", "busy");
    esp_err_t err = appliance_forget();
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "busy");
    if (err == ESP_ERR_NOT_SUPPORTED) return send_error(req, "501 Not Implemented", "not_supported");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

// --- Rezepte und Fotos ----------------------------------------------------------
//
// Alles wird in Blöcken gestreamt: Antworten als HTTP-Chunks direkt aus den Dateien, Fotos beim
// Hochladen direkt in die Datei. Im RAM liegt höchstens ein Rezept (≤ 16 KB) samt cJSON-Baum.

#define STREAM_BLOCK 1024

// Letzten Pfadteil nach `prefix` lesen (".../recipes/<id>", ".../media/<sha>").
static bool path_tail(httpd_req_t *req, const char *prefix, char *out, size_t len)
{
    size_t n = strlen(prefix);
    if (strncmp(req->uri, prefix, n) != 0) return false;
    const char *s = req->uri + n;
    size_t k = strcspn(s, "?/");
    if (k == 0 || k >= len || s[k] == '/') return false;
    memcpy(out, s, k);
    out[k] = '\0';
    return true;
}

// Datei als Chunks senden; buf hat STREAM_BLOCK Byte. false, wenn die Verbindung weg ist.
static bool send_file_chunks(httpd_req_t *req, const char *path, char *buf)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = true;
    size_t n;
    while (ok && (n = fread(buf, 1, STREAM_BLOCK, f)) > 0) ok = httpd_resp_send_chunk(req, buf, n) == ESP_OK;
    fclose(f);
    return ok;
}

// "If-Match: 7" (auch "7" in Anführungszeichen wie ein ETag). -1 = kein Header, -2 = ungültig.
static int64_t if_match_header(httpd_req_t *req)
{
    char v[24];
    if (httpd_req_get_hdr_value_str(req, "If-Match", v, sizeof(v)) != ESP_OK) return -1;
    char *p = v;
    if (*p == '"') p++;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(p, &end, 10);
    if (end == p || errno || n > UINT32_MAX || (*end && strcmp(end, "\"") != 0)) return -2;
    return (int64_t)n;
}

static void broadcast_recipes(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "rev", recipes_rev());
    broadcast("recipes", d);
}

typedef struct {
    httpd_req_t *req;
    uint32_t since;
    bool deleted;  // zweiter Durchlauf: Löschungen
    bool first;
    bool ok;
    char *buf;
} list_ctx_t;

static bool list_visit(const recipes_entry_t *e, void *arg)
{
    list_ctx_t *c = arg;
    if (e->rev <= c->since || e->deleted != c->deleted) return true;
    if (!c->first) c->ok = httpd_resp_send_chunk(c->req, ",", 1) == ESP_OK;
    c->first = false;
    if (c->ok && c->deleted) {
        int n = snprintf(c->buf, STREAM_BLOCK, "\"%s\"", e->id);
        c->ok = httpd_resp_send_chunk(c->req, c->buf, n) == ESP_OK;
    } else if (c->ok) {
        char path[96];
        recipes_path(e, path, sizeof(path));
        c->ok = send_file_chunks(c->req, path, c->buf);
    }
    return c->ok;
}

// GET /api/v1/recipes?since=<rev> → {"rev":n,"changed":[ganze Rezepte],"deleted":[ids]}
static esp_err_t h_recipes(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char q[48], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    list_ctx_t c = {.req = req, .since = since, .first = true, .ok = true, .buf = malloc(STREAM_BLOCK)};
    if (!c.buf) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    int n = snprintf(c.buf, STREAM_BLOCK, "{\"rev\":%" PRIu32 ",\"changed\":[", recipes_rev());
    c.ok = httpd_resp_send_chunk(req, c.buf, n) == ESP_OK;
    if (c.ok) recipes_foreach(list_visit, &c);
    if (c.ok) c.ok = httpd_resp_send_chunk(req, "],\"deleted\":[", 13) == ESP_OK;
    // Ohne Stand (since = 0) braucht die App keine Löschungen.
    c.deleted = true;
    c.first = true;
    if (c.ok && since > 0) recipes_foreach(list_visit, &c);
    if (c.ok) c.ok = httpd_resp_send_chunk(req, "]}", 2) == ESP_OK;
    if (c.ok) httpd_resp_send_chunk(req, NULL, 0);
    free(c.buf);
    return c.ok ? ESP_OK : ESP_FAIL;  // ESP_FAIL schließt die halb gesendete Antwort
}

static esp_err_t h_recipe_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char id[RECIPES_ID_MAX + 1];
    recipes_entry_t e;
    if (!path_tail(req, "/api/v1/recipes/", id, sizeof(id)) || recipes_find(id, &e) != ESP_OK) {
        return send_error(req, "404 Not Found", "recipe");
    }
    if (e.deleted) return send_error(req, "404 Not Found", "deleted");
    char etag[16], path[96];
    snprintf(etag, sizeof(etag), "\"%" PRIu32 "\"", e.rev);
    recipes_path(&e, path, sizeof(path));
    char *buf = malloc(STREAM_BLOCK);
    if (!buf) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "ETag", etag);
    bool ok = send_file_chunks(req, path, buf);
    free(buf);
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t recipe_result(httpd_req_t *req, esp_err_t err, const char *id, const recipes_result_t *r)
{
    cJSON *o = cJSON_CreateObject();
    switch (err) {
    case ESP_OK:
        broadcast_recipes();
        cJSON_AddStringToObject(o, "id", id);
        cJSON_AddNumberToObject(o, "rev", r->rev);
        if (r->deleted) cJSON_AddBoolToObject(o, "deleted", true);
        else cJSON_AddStringToObject(o, "updated", r->updated);
        return send_json(req, "200 OK", o);
    case ESP_ERR_INVALID_STATE:  // jemand anderes hat inzwischen geändert oder gelöscht
        cJSON_AddStringToObject(o, "error", "rev");
        cJSON_AddNumberToObject(o, "rev", r->rev);
        cJSON_AddBoolToObject(o, "deleted", r->deleted);
        return send_json(req, "412 Precondition Failed", o);
    case ESP_ERR_INVALID_ARG:
        cJSON_AddStringToObject(o, "error", r->error ? r->error : "json");
        return send_json(req, "400 Bad Request", o);
    case ESP_ERR_INVALID_SIZE:
        cJSON_AddStringToObject(o, "error", "size");
        return send_json(req, "413 Payload Too Large", o);
    case ESP_ERR_NOT_FOUND:
        cJSON_AddStringToObject(o, "error", "recipe");
        return send_json(req, "404 Not Found", o);
    case ESP_ERR_NO_MEM:
        cJSON_AddStringToObject(o, "error", "memory");
        return send_json(req, "503 Service Unavailable", o);
    default:
        cJSON_AddStringToObject(o, "error", "storage");
        return send_json(req, "500 Internal Server Error", o);
    }
}

// Body in einen frisch angelegten Puffer mit `extra` Byte Reserve lesen (NULL = Antwort schon gesendet).
static char *recv_raw(httpd_req_t *req, size_t extra)
{
    char *buf = malloc(req->content_len + extra);
    if (!buf) {
        send_error(req, "503 Service Unavailable", "memory");
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) {
            free(buf);
            send_error(req, "400 Bad Request", "body");
            return NULL;
        }
        got += n;
    }
    return buf;
}

static esp_err_t h_recipe_put(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char id[RECIPES_ID_MAX + 1];
    if (!path_tail(req, "/api/v1/recipes/", id, sizeof(id)) || !recipes_valid_id(id)) {
        return send_error(req, "400 Bad Request", "id");
    }
    int64_t match = if_match_header(req);
    if (match == -2) return send_error(req, "400 Bad Request", "if_match");
    if (req->content_len == 0) return send_error(req, "400 Bad Request", "body");
    if (req->content_len > RECIPES_MAX_JSON) return send_error(req, "413 Payload Too Large", "size");
    char *body = recv_raw(req, RECIPES_JSON_SLACK);
    if (!body) return ESP_OK;
    recipes_result_t r;
    esp_err_t err = recipes_put(id, body, req->content_len, req->content_len + RECIPES_JSON_SLACK, match, &r);
    return recipe_result(req, err, id, &r);
}

static esp_err_t h_recipe_delete(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char id[RECIPES_ID_MAX + 1];
    if (!path_tail(req, "/api/v1/recipes/", id, sizeof(id))) return send_error(req, "404 Not Found", "recipe");
    int64_t match = if_match_header(req);
    if (match == -2) return send_error(req, "400 Bad Request", "if_match");
    recipes_result_t r;
    esp_err_t err = recipes_delete(id, match, &r);
    return recipe_result(req, err, id, &r);
}

static esp_err_t h_media_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char sha[RECIPES_SHA_HEX + 1], path[96];
    struct stat st;
    if (!path_tail(req, "/api/v1/media/", sha, sizeof(sha)) || !recipes_media_valid(sha)) {
        return send_error(req, "404 Not Found", "media");
    }
    recipes_media_path(sha, path, sizeof(path));
    if (stat(path, &st) != 0) return send_error(req, "404 Not Found", "media");
    char *buf = malloc(STREAM_BLOCK);
    if (!buf) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "image/webp");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=31536000, immutable");
    bool ok = send_file_chunks(req, path, buf);
    free(buf);
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    return ok ? ESP_OK : ESP_FAIL;
}

// Body = WebP (application/octet-stream oder image/webp), Name = SHA-256 des Inhalts.
// Blockiert den Server für die Dauer des Uploads (1 MB im Heim-WLAN: ca. 1–3 s).
static esp_err_t h_media_put(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char sha[RECIPES_SHA_HEX + 1], path[96];
    struct stat st;
    if (!path_tail(req, "/api/v1/media/", sha, sizeof(sha)) || !recipes_media_valid(sha)) {
        return send_error(req, "400 Bad Request", "sha256");
    }
    recipes_media_path(sha, path, sizeof(path));
    if (stat(path, &st) == 0) {
        // Schon da (gleicher Inhalt). Den Rest des Bodys verwirft der Server.
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "sha256", sha);
        return send_json(req, "200 OK", o);
    }
    recipes_upload_t *u;
    esp_err_t err = recipes_media_begin(sha, req->content_len, &u);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, req->content_len ? "413 Payload Too Large" : "400 Bad Request", "size");
    }
    if (err == ESP_ERR_NO_MEM) return send_error(req, "507 Insufficient Storage", "full");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "storage");

    char *buf = malloc(STREAM_BLOCK * 2);
    size_t left = req->content_len;
    int timeouts = 0;
    while (buf && left > 0) {
        int n = httpd_req_recv(req, buf, left < STREAM_BLOCK * 2 ? left : STREAM_BLOCK * 2);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) break;
        timeouts = 0;
        if (recipes_media_write(u, buf, n) != ESP_OK) break;
        left -= n;
    }
    free(buf);
    if (left > 0) {
        recipes_media_abort(u);
        return send_error(req, "400 Bad Request", "upload");
    }
    const char *code;
    if (recipes_media_finish(u, &code) != ESP_OK) {
        return send_error(req, strcmp(code, "storage") ? "400 Bad Request" : "500 Internal Server Error", code);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "sha256", sha);
    return send_json(req, "201 Created", o);
}

// --- Firmware-Update ----------------------------------------------------------

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

// Body = Firmware-Image (application/octet-stream), Header X-Sha256 + X-Signature (hex), siehe docs/updates.md.
// Blockiert den Server für die Dauer des Uploads (ca. 30 s) – Fortschritt zählt die App selbst.
static esp_err_t h_ota(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char hex[2 * UPDATE_SIG_LEN + 1];
    uint8_t sha[32], sig[UPDATE_SIG_LEN];
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", hex, sizeof(hex)) != ESP_OK || !hex_to_bytes(hex, sha, 32) ||
        httpd_req_get_hdr_value_str(req, "X-Signature", hex, sizeof(hex)) != ESP_OK ||
        !hex_to_bytes(hex, sig, UPDATE_SIG_LEN)) {
        return send_error(req, "400 Bad Request", "header");
    }
    appliance_status_t st;
    appliance_get_status(&st);
    if (sequencer_active() || st.active) return send_error(req, "409 Conflict", "busy");

    esp_err_t err = update_begin(req->content_len);
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, "413 Payload Too Large", "size");
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "busy");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(err));

    const size_t chunk = 4096;
    char *buf = malloc(chunk);
    size_t left = req->content_len;
    int timeouts = 0;
    while (buf && left > 0) {
        int n = httpd_req_recv(req, buf, left < chunk ? left : chunk);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) break;
        timeouts = 0;
        if (update_write(buf, n) != ESP_OK) break;
        left -= n;
    }
    free(buf);
    if (left > 0) {
        update_abort();
        return send_error(req, "400 Bad Request", "upload");
    }
    const char *code;
    if (update_finish(sha, sig, &code) != ESP_OK) return send_error(req, "400 Bad Request", code);
    update_restart_later(1000);
    return send_json(req, "202 Accepted", cJSON_CreateObject());
}

static esp_err_t h_reset(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    bool wipe = false;
    if (req->content_len) {
        cJSON *b = read_body(req);
        if (!b) return ESP_OK;
        wipe = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "wipe"));
        cJSON_Delete(b);
    }
    ESP_LOGW(TAG, "Werksreset aus der App%s", wipe ? " - alles loeschen" : "");
    if (wipe) factory_wipe_later(500);
    else factory_reset_later(500);
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

// --- Diagnose („Problem melden“) ----------------------------------------------------

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

// Log als JSON-String-Inhalt escapen (ohne Anführungszeichen). out braucht 6 × n Byte.
static size_t json_escape(const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c == '\r') {
            continue;
        } else if (c < 0x20 || c == 0x7f) {
            o += sprintf(out + o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    return o;
}

#define DIAG_READ 256

// GET /api/v1/diag → Zustand der Brücke + letzte Log-Ausgaben (Zugangsdaten maskiert, siehe diag.h).
// Das Log wird in Stücken direkt aus dem Ringpuffer gestreamt, damit kein zweiter 8-KB-Puffer nötig ist.
static esp_err_t h_diag(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    uint32_t pos, end;
    diag_log_span(&pos, &end);  // Stand jetzt festhalten – was beim Senden dazukommt, gehört nicht mehr dazu
    bool wrapped = end - pos >= CONFIG_RL_DIAG_LOG_SIZE;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(o, "uptimeS", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "resetReason", reset_reason_name(esp_reset_reason()));
    cJSON *h = cJSON_AddObjectToObject(o, "heap");
    cJSON_AddNumberToObject(h, "free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(h, "minFree", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(h, "internalFree", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "internalMinFree", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "largestBlock", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(w, "configured", net_home_configured());
    cJSON_AddBoolToObject(w, "connected", net_home_connected());
    int rssi;
    if (net_home_rssi(&rssi)) cJSON_AddNumberToObject(w, "rssi", rssi);
    else cJSON_AddNullToObject(w, "rssi");
    const char *werr = net_home_error();
    if (werr) cJSON_AddStringToObject(w, "error", werr);
    else cJSON_AddNullToObject(w, "error");
    appliance_status_t st;
    appliance_get_status(&st);
    cJSON *a = cJSON_AddObjectToObject(o, "appliance");
    cJSON_AddBoolToObject(a, "connected", st.connected);
    cJSON_AddBoolToObject(a, "active", st.active);
    sequencer_info_t si;
    sequencer_get(&si);
    cJSON_AddStringToObject(o, "sequence", sequencer_state_name(si.state));
    cJSON *ota = cJSON_AddObjectToObject(o, "ota");
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(ota, "running", run ? run->label : "?");
    esp_ota_img_states_t ost;
    cJSON_AddBoolToObject(ota, "pendingVerify",
                          run && esp_ota_get_state_partition(run, &ost) == ESP_OK && ost == ESP_OTA_IMG_PENDING_VERIFY);
    cJSON_AddBoolToObject(o, "logTruncated", wrapped);

    char *head = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char *buf = malloc(DIAG_READ + DIAG_READ * 6 + 1);
    if (!head || !buf) {
        free(head);
        free(buf);
        return httpd_resp_send_500(req);
    }
    char *esc = buf + DIAG_READ;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t hl = strlen(head);
    head[hl - 1] = '\0';  // schließende Klammer weg, "log" kommt dahinter
    bool ok = httpd_resp_send_chunk(req, head, hl - 1) == ESP_OK &&
              httpd_resp_send_chunk(req, ",\"log\":\"", 8) == ESP_OK;
    free(head);
    bool skip_partial = wrapped;  // erste Zeile ist nach dem Überlauf meist abgeschnitten
    size_t n;
    while (ok && (n = diag_log_read(&pos, end, buf, DIAG_READ)) > 0) {
        const char *p = buf;
        if (skip_partial) {
            const char *nl = memchr(buf, '\n', n);
            if (!nl) continue;
            skip_partial = false;
            n -= (size_t)(nl + 1 - buf);
            p = nl + 1;
        }
        size_t el = json_escape(p, n, esc);
        if (el) ok = httpd_resp_send_chunk(req, esc, el) == ESP_OK;
    }
    if (ok) ok = httpd_resp_send_chunk(req, "\"}", 2) == ESP_OK;
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return ok ? ESP_OK : ESP_FAIL;
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
    if (r == AUTH_PAIR_BUSY) {
        cJSON_AddStringToObject(o, "error", "busy");
        httpd_resp_set_hdr(req, "Retry-After", "60");
        return send_json(req, "429 Too Many Requests", o);
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

void api_issue_token(char *out, size_t len) { auth_new_token(out, len); }

// Die App-API gibt es nur im Heim-WLAN. Verbindungen über das WLAN der Maschine (AP, NET_AP_IP)
// werden gleich beim Öffnen geschlossen – dort hat nur die Maschine etwas zu suchen (EN 18031 GEC).
static bool local_is_ap(int sockfd)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(sockfd, (struct sockaddr *)&local, &len) != 0) return false;
    uint32_t ap = ipaddr_addr(NET_AP_IP);
    if (local.ss_family == AF_INET) return ((struct sockaddr_in *)&local)->sin_addr.s_addr == ap;
#if CONFIG_LWIP_IPV6
    // httpd lauscht mit IPv6 auf beiden Protokollen: IPv4 kommt dann als ::ffff:a.b.c.d
    if (local.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        return memcmp(b, mapped, sizeof(mapped)) == 0 && memcmp(b + 12, &ap, 4) == 0;
    }
#endif
    return false;
}

static esp_err_t on_open(httpd_handle_t hd, int sockfd)
{
    if (local_is_ap(sockfd)) {
        ESP_LOGW(TAG, "Verbindung ueber das Maschinen-WLAN abgelehnt");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t api_start(void)
{
    auth_init(on_pair_request);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = MAX_CLIENTS - 1;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 32;  // Waage, Rezepte, Fotos
    cfg.uri_match_fn = httpd_uri_match_wildcard;  // /recipes/{id}, /media/{sha256}
    cfg.stack_size = 8192;  // Signaturprüfung beim Update
    cfg.open_fn = on_open;  // nur Heim-WLAN
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
        {.uri = "/api/v1/scale/watch", .method = HTTP_POST, .handler = h_scale_watch},
        {.uri = "/api/v1/scale/tare", .method = HTTP_POST, .handler = h_scale_tare},
        {.uri = "/api/v1/machine/onboard", .method = HTTP_POST, .handler = h_onboard},
        {.uri = "/api/v1/machine/repair", .method = HTTP_POST, .handler = h_repair},
        {.uri = "/api/v1/sequence/start", .method = HTTP_POST, .handler = h_seq_start},
        {.uri = "/api/v1/sequence/abort", .method = HTTP_POST, .handler = h_seq_abort},
        {.uri = "/api/v1/sequence", .method = HTTP_GET, .handler = h_seq_get},
        {.uri = "/api/v1/programs", .method = HTTP_GET, .handler = h_programs},
        {.uri = "/api/v1/recipes", .method = HTTP_GET, .handler = h_recipes},
        {.uri = "/api/v1/recipes/*", .method = HTTP_GET, .handler = h_recipe_get},
        {.uri = "/api/v1/recipes/*", .method = HTTP_PUT, .handler = h_recipe_put},
        {.uri = "/api/v1/recipes/*", .method = HTTP_DELETE, .handler = h_recipe_delete},
        {.uri = "/api/v1/media/*", .method = HTTP_GET, .handler = h_media_get},
        {.uri = "/api/v1/media/*", .method = HTTP_PUT, .handler = h_media_put},
        {.uri = "/api/v1/time", .method = HTTP_POST, .handler = h_time},
        {.uri = "/api/v1/diag", .method = HTTP_GET, .handler = h_diag},
        {.uri = "/api/v1/reset", .method = HTTP_POST, .handler = h_reset},
        {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/v1/pair", .method = HTTP_POST, .handler = h_pair},
        {.uri = "/api/v1/pair/approve", .method = HTTP_POST, .handler = h_pair_approve},
        {.uri = "/api/v1/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
         .ws_pre_handshake_cb = ws_pre_handshake},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);

    esp_event_handler_register(APPLIANCE_EVENT, ESP_EVENT_ANY_ID, on_appliance_event, NULL);
    esp_event_handler_register(SEQUENCER_EVENT, ESP_EVENT_ANY_ID, on_sequencer_event, NULL);
    ESP_LOGI(TAG, "App-API auf Port 80 bereit");
    return ESP_OK;
}
