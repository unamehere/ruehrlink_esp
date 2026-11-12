#include "provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_srp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"

#include "api.h"
#include "led.h"
#include "net.h"

static const char *TAG = "provision";

#define WINDOW_US     (10 * 60 * 1000000LL)  // Bluetooth nach dem Einschalten (Heim-WLAN schon eingerichtet)
#define POP_LEN       10
#define MAX_SCAN      15
#define SRP_USER      "freikoch"
#define SALT_LEN      16

// Nur die verschlüsselte Sitzung (Security 2, SRP6a) kommt vom ESP-IDF-Paket (SESSION_ONLY-Modus):
// Es fasst das WLAN nicht an – das Maschinen-WLAN muss weiterlaufen. WLAN-Suche, Zugangsdaten
// und Token laufen über eigene JSON-Endpunkte (Protokoll: docs/architektur.md).

static char s_pop[POP_LEN + 1];
static char s_salt[SALT_LEN];
static char *s_verifier;
static network_prov_security2_params_t s_sec2;
static char s_name[24];
static esp_timer_handle_t s_stop_timer;
static bool s_running;

// ---------------------------------------------------------------------------
// Einrichtungs-Code (Proof of Possession), pro Gerät einmal erzeugt. Für Security 2 speichern wir
// zusätzlich Salt und Verifier (SRP6a, Benutzer "freikoch") – die Berechnung dauert einige Sekunden.

static esp_err_t load_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("prov", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_pop), slen = SALT_LEN, vlen = 0;
    bool have = nvs_get_str(h, "pop", s_pop, &len) == ESP_OK &&
                nvs_get_blob(h, "salt", s_salt, &slen) == ESP_OK &&
                nvs_get_blob(h, "verifier", NULL, &vlen) == ESP_OK && vlen > 0;
    if (have) {
        s_verifier = malloc(vlen);
        err = s_verifier ? nvs_get_blob(h, "verifier", s_verifier, &vlen) : ESP_ERR_NO_MEM;
    } else {
        static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
        for (int i = 0; i < POP_LEN; i++) s_pop[i] = a[esp_random() % (sizeof(a) - 1)];
        s_pop[POP_LEN] = '\0';
        char *salt = NULL;
        int vl = 0;
        ESP_LOGI(TAG, "erzeuge Einrichtungs-Schluessel (einmalig) ...");
        err = esp_srp_gen_salt_verifier(SRP_USER, strlen(SRP_USER), s_pop, POP_LEN, &salt, SALT_LEN, &s_verifier, &vl);
        if (err == ESP_OK) {
            memcpy(s_salt, salt, SALT_LEN);
            free(salt);
            vlen = vl;
            nvs_set_str(h, "pop", s_pop);
            nvs_set_blob(h, "salt", s_salt, SALT_LEN);
            nvs_set_blob(h, "verifier", s_verifier, vlen);
            nvs_commit(h);
        }
    }
    nvs_close(h);
    s_sec2 = (network_prov_security2_params_t){
        .salt = s_salt, .salt_len = SALT_LEN, .verifier = s_verifier, .verifier_len = (uint16_t)vlen};
    return err;
}

// ---------------------------------------------------------------------------
// Endpunkte (Ein- und Ausgabe JSON, über die verschlüsselte Sitzung)

static esp_err_t reply(cJSON *o, uint8_t **outbuf, ssize_t *outlen)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return ESP_ERR_NO_MEM;
    *outbuf = (uint8_t *)s;  // protocomm gibt den Puffer mit free() frei
    *outlen = strlen(s);
    return ESP_OK;
}

static esp_err_t ep_info(uint32_t sid, const uint8_t *in, ssize_t inlen, uint8_t **out, ssize_t *outlen, void *p)
{
    char ssid[33], host[40];
    net_home_ssid(ssid, sizeof(ssid));
    snprintf(host, sizeof(host), "freikoch-%c%c%c%c.local", net_device_id()[0] | 0x20, net_device_id()[1] | 0x20,
             net_device_id()[2] | 0x20, net_device_id()[3] | 0x20);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", net_device_id());
    cJSON_AddStringToObject(o, "name", "Freikoch Brücke");
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "host", host);
    cJSON *home = cJSON_AddObjectToObject(o, "home");
    cJSON_AddBoolToObject(home, "configured", net_home_configured());
    cJSON_AddBoolToObject(home, "connected", net_home_connected());
    cJSON_AddStringToObject(home, "ssid", ssid);
    const char *err = net_home_error();
    if (err) cJSON_AddStringToObject(home, "error", err);
    else cJSON_AddNullToObject(home, "error");
    return reply(o, out, outlen);
}

static esp_err_t ep_scan(uint32_t sid, const uint8_t *in, ssize_t inlen, uint8_t **out, ssize_t *outlen, void *p)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    cJSON *list = cJSON_CreateArray();
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 30;
        wifi_ap_record_t *r = calloc(n, sizeof(*r));
        if (r && esp_wifi_scan_get_ap_records(&n, r) == ESP_OK) {
            int added = 0;
            for (int i = 0; i < n && added < MAX_SCAN; i++) {  // nach Signal sortiert; doppelte SSIDs weglassen
                const char *ss = (const char *)r[i].ssid;
                if (!ss[0] || strncmp(ss, "Freikoch-", 9) == 0 || strncmp(ss, "KENWOOD", 7) == 0) continue;
                bool dup = false;
                for (int j = 0; j < i && !dup; j++) dup = strcmp(ss, (const char *)r[j].ssid) == 0;
                if (dup) continue;
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "ssid", ss);
                cJSON_AddNumberToObject(e, "rssi", r[i].rssi);
                cJSON_AddBoolToObject(e, "secure", r[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(list, e);
                added++;
            }
        }
        free(r);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "networks", list);
    return reply(o, out, outlen);
}

static esp_err_t ep_wifi(uint32_t sid, const uint8_t *in, ssize_t inlen, uint8_t **out, ssize_t *outlen, void *p)
{
    cJSON *b = cJSON_ParseWithLength((const char *)in, inlen);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(b, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(b, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
        err = net_home_set(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
        ESP_LOGI(TAG, "Heim-WLAN per Bluetooth gesetzt: %s", ssid->valuestring);
    }
    cJSON_Delete(b);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);  // Ergebnis der Verbindung: fk-info abfragen
    return reply(o, out, outlen);
}

static esp_err_t ep_token(uint32_t sid, const uint8_t *in, ssize_t inlen, uint8_t **out, ssize_t *outlen, void *p)
{
    char token[40];
    api_issue_token(token, sizeof(token));
    ESP_LOGI(TAG, "App per Bluetooth gekoppelt");
    led_signal(LED_OK);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "token", token);
    return reply(o, out, outlen);
}

// ---------------------------------------------------------------------------

static void stop_cb(void *arg)
{
    if (!net_home_connected()) {  // ohne Heim-WLAN bleibt Bluetooth an
        esp_timer_start_once(s_stop_timer, 60 * 1000000LL);
        return;
    }
    network_prov_mgr_deinit();  // gibt den Bluetooth-Speicher frei (bis zum nächsten Einschalten)
    s_running = false;
    ESP_LOGI(TAG, "Bluetooth-Einrichtung beendet");
}

esp_err_t provision_start(void)
{
    esp_err_t err = load_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Schluessel: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(s_name, sizeof(s_name), "Freikoch-%s", net_device_id());

    network_prov_mgr_config_t cfg = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .mode = NETWORK_PROV_MODE_SESSION_ONLY,
    };
    err = network_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(err));
        return err;
    }
    const char *eps[] = {"fk-info", "fk-scan", "fk-wifi", "fk-token"};
    for (int i = 0; i < 4; i++) network_prov_mgr_endpoint_create(eps[i]);

    err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, (const void *)&s_sec2, s_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        network_prov_mgr_deinit();
        return err;
    }
    network_prov_mgr_endpoint_register("fk-info", ep_info, NULL);
    network_prov_mgr_endpoint_register("fk-scan", ep_scan, NULL);
    network_prov_mgr_endpoint_register("fk-wifi", ep_wifi, NULL);
    network_prov_mgr_endpoint_register("fk-token", ep_token, NULL);
    s_running = true;

    const esp_timer_create_args_t t = {.callback = stop_cb, .name = "prov_stop"};
    esp_timer_create(&t, &s_stop_timer);
    esp_timer_start_once(s_stop_timer, WINDOW_US);
    if (!net_home_configured()) led_signal(LED_SETUP);

    ESP_LOGI(TAG, "Bluetooth-Einrichtung aktiv als %s", s_name);
#if CONFIG_FK_DEVELOPER_MODE
    // Inhalt des QR-Codes auf dem Etikett (nur Entwicklermodus ausgeben)
    ESP_LOGI(TAG, "QR: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\",\"security\":2,\"username\":\"" SRP_USER "\"}", s_name, s_pop);
#endif
    return ESP_OK;
}
