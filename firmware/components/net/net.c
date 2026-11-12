#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "nvs.h"

#include "net_priv.h"

static const char *TAG = "net";

#define NVS_NS "net"
#define AP_PASS_LEN 12
#define RECONNECT_MIN_MS 2000
#define RECONNECT_MAX_MS 60000

static char s_id[5];               // "98C0"
static char s_ap_ssid[33];         // "Freikoch-98C0"
static char s_ap_pass[64];
static char s_hostname[20];        // "freikoch-98c0"

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static volatile bool s_home_connected;
static volatile bool s_home_paused;

// ---------------------------------------------------------------------------
// Identität und AP-Passwort

static void init_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Freikoch-%s", s_id);
    snprintf(s_hostname, sizeof(s_hostname), "freikoch-%02x%02x", mac[4], mac[5]);
}

// Pro Gerät zufällig, beim ersten Start erzeugt (siehe docs/architektur.md).
static void load_ap_password(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    size_t len = sizeof(s_ap_pass);
    if (nvs_get_str(h, "ap_pass", s_ap_pass, &len) != ESP_OK) {
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // ohne l/1/o/0
        for (int i = 0; i < AP_PASS_LEN; i++) {
            s_ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        }
        s_ap_pass[AP_PASS_LEN] = '\0';
        ESP_ERROR_CHECK(nvs_set_str(h, "ap_pass", s_ap_pass));
        ESP_ERROR_CHECK(nvs_commit(h));
        ESP_LOGI(TAG, "neues AP-Passwort erzeugt");
    }
    nvs_close(h);
}

// Entwicklung: Zugangsdaten, die tools/dev_nvs.py direkt in den NVS des Geräts geschrieben hat.
// Sie liegen nie in einer Datei im Repo. Ab der Bluetooth-Einrichtung nicht mehr nötig.
//   dev_ap_ssid / dev_ap_pass     Maschinen-WLAN überschreiben (Maschine schon anders gekoppelt)
//   dev_home_ssid / dev_home_pass Heim-WLAN einmalig übernehmen (danach gelöscht)
static void load_dev_overrides(char *home_ssid, size_t ssid_len, char *home_pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char ssid[33];
    size_t len = sizeof(ssid);
    if (nvs_get_str(h, "dev_ap_ssid", ssid, &len) == ESP_OK && ssid[0]) {
        size_t plen = sizeof(s_ap_pass);
        if (nvs_get_str(h, "dev_ap_pass", s_ap_pass, &plen) == ESP_OK) {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
            ESP_LOGW(TAG, "Entwicklung: Maschinen-WLAN aus NVS (%s)", s_ap_ssid);
        }
    }
    len = ssid_len;
    if (nvs_get_str(h, "dev_home_ssid", home_ssid, &len) == ESP_OK) {
        len = pass_len;
        if (nvs_get_str(h, "dev_home_pass", home_pass, &len) != ESP_OK) home_pass[0] = '\0';
        nvs_erase_key(h, "dev_home_ssid");
        nvs_erase_key(h, "dev_home_pass");
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Heim-WLAN: Verbinden mit Backoff, Pause für die Maschinen-Kopplung

static void connect_home(void)
{
    if (s_home_paused || !net_home_configured()) return;
    esp_wifi_connect();
}

static void reconnect_cb(void *arg)
{
    s_home_paused = false;
    connect_home();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        connect_home();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        s_home_connected = false;
        if (s_home_paused) break;
        ESP_LOGW(TAG, "Heim-WLAN getrennt (Grund %d), neuer Versuch in %lu s", e->reason,
                 (unsigned long)(s_backoff_ms / 1000));
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : s_backoff_ms * 2;
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "Geraet im Maschinen-WLAN: " MACSTR, MAC2STR(e->mac));
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "Geraet hat Maschinen-WLAN verlassen: " MACSTR, MAC2STR(e->mac));
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_home_connected = true;
        s_backoff_ms = RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "Heim-WLAN verbunden: " IPSTR " (%s.local)", IP2STR(&e->ip_info.ip), s_hostname);
    } else if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "Maschinen-WLAN vergibt " IPSTR, IP2STR(&e->ip));
    }
}

bool net_home_configured(void)
{
    wifi_config_t cfg = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

bool net_home_connected(void) { return s_home_connected; }

esp_err_t net_home_set(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    if (strlen(ssid) >= sizeof(cfg.sta.ssid) || strlen(password) >= sizeof(cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    strcpy((char *)cfg.sta.ssid, ssid);
    strcpy((char *)cfg.sta.password, password);
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);  // wird in NVS gespeichert
    if (err != ESP_OK) return err;
    s_backoff_ms = RECONNECT_MIN_MS;
    connect_home();
    return ESP_OK;
}

void net_home_pause(uint32_t seconds)
{
    esp_timer_stop(s_reconnect_timer);
    if (seconds == 0) {
        s_home_paused = false;
        connect_home();
        return;
    }
    ESP_LOGI(TAG, "Heim-WLAN pausiert fuer max. %lu s (Kanal halten)", (unsigned long)seconds);
    s_home_paused = true;
    esp_wifi_disconnect();
    esp_timer_start_once(s_reconnect_timer, (uint64_t)seconds * 1000000);
}

// ---------------------------------------------------------------------------
// Start

static void start_ap_netif(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ipaddr_addr(NET_AP_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = ipaddr_addr("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));

    // DHCP soll uns selbst als DNS-Server anbieten
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer_dns = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
}

static void start_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set("Freikoch Bruecke");
    mdns_txt_item_t txt[] = {
        {"id", s_id},
        {"fw", esp_app_get_description()->version},
        {"api", "1"},
    };
    mdns_service_add(NULL, "_freikoch", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
}

void net_mdns_set_txt(const char *key, const char *value)
{
    mdns_service_txt_item_set("_freikoch", "_tcp", key, value);
}

esp_err_t net_start(void)
{
    init_identity();
    load_ap_password();
    char dev_ssid[33] = "", dev_pass[65] = "";
    load_dev_overrides(dev_ssid, sizeof(dev_ssid), dev_pass, sizeof(dev_pass));

    ESP_ERROR_CHECK(esp_netif_init());
    start_ap_netif();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, s_hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));

    const esp_timer_create_args_t targs = {.callback = reconnect_cb, .name = "wifi_reconn"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.capable = true},
        },
    };
    strcpy((char *)ap.ap.ssid, s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strcpy((char *)ap.ap.password, s_ap_pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    if (dev_ssid[0]) {
        ESP_LOGW(TAG, "Entwicklung: Heim-WLAN aus NVS uebernommen");
        net_home_set(dev_ssid, dev_pass);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    dns_start(ipaddr_addr(NET_AP_IP));
    start_mdns();

    ESP_LOGI(TAG, "Maschinen-WLAN %s (IP %s), Heim-WLAN %s", s_ap_ssid, NET_AP_IP,
             net_home_configured() ? "eingerichtet" : "noch nicht eingerichtet");
    return ESP_OK;
}

const char *net_device_id(void) { return s_id; }
const char *net_ap_ssid(void) { return s_ap_ssid; }
const char *net_ap_password(void) { return s_ap_pass; }
