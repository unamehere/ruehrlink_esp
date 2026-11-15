// Waage der KCL95
//
// Die Maschine betreibt selbst einen WebSocket-Server (wss://<Maschine>:443/, TLS 1.2, selbstsigniertes
// Zertifikat), der nur sendet – etwa 20-mal pro Sekunde:
//   {"event":{"timestamp":…,"type":"weightUpdate","value":180.0,"unit":"gram"}}
// Befehle nimmt er nicht an (recherche/kenwood-cooking-chef-setup/PROTOCOL.md, Abschnitt 4). Tara rechnet
// deshalb appliance.c.
//
// Die Verbindung kostet eine zweite TLS-Sitzung (interner RAM, Ziel ESP32-C6 ohne PSRAM) und läuft daher nur
// bei Bedarf: solange eine App die Waage anzeigt (Lease, von der App erneuert) oder kurz nachdem der Kopf
// hochgeklappt wurde. Danach endet der Task und gibt alles frei.
//
// Zertifikat: beim ersten erfolgreichen Kontakt gemerkt (NVS) und danach Byte für Byte verlangt
// (Trust on first use). Das Maschinen-WLAN gehört uns, ein fremder Server müsste sich dort erst einschleichen.

#include "scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

#include "appliance.h"

static const char *TAG = "waage";

#define PORT          443
#define FRAME_MAX     512                    // längere Nachrichten werden verworfen
#define SILENCE_US    (10 * 1000000LL)       // so lange nichts gehört → Verbindung gilt als tot
#define RETRY_MS      3000
#define RCV_TIMEOUT_S 1                      // Lesen kehrt spätestens so oft zurück (Lease prüfen)
#define NVS_NS        "waage"
#define NVS_KEY       "cert"

extern const uint8_t setup_cert_start[] asm("_binary_setup_server_pem_start");
extern const uint8_t setup_cert_end[] asm("_binary_setup_server_pem_end");

static volatile uint32_t s_host;
static volatile int64_t s_until_us;
static bool s_running;  // Task läuft (geschützt durch s_mux)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool needed(void) { return esp_timer_get_time() < s_until_us; }

// ---------------------------------------------------------------------------
// Zertifikat (Trust on first use)

static mbedtls_x509_crt s_chain;  // nur Platzhalter: mbedTLS verlangt eine CA-Kette, geprüft wird in verify_cb
static bool s_chain_loaded;
static uint8_t *s_pin;
static size_t s_pin_len;
static bool s_pin_loaded;

static void load_pin(void)
{
    if (s_pin_loaded) return;
    s_pin_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0 && (s_pin = malloc(len))) {
        if (nvs_get_blob(h, NVS_KEY, s_pin, &len) == ESP_OK) s_pin_len = len;
        else {
            free(s_pin);
            s_pin = NULL;
        }
    }
    nvs_close(h);
}

static void store_pin(const uint8_t *der, size_t len)
{
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, der, len);
    s_pin = copy;
    s_pin_len = len;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, der, len);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Zertifikat der Waage gemerkt (%u Byte)", (unsigned)len);
}

static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    if (depth > 0) {  // Zwischenzertifikate egal, es zählt nur das der Maschine
        *flags = 0;
        return 0;
    }
    if (!s_pin) {
        store_pin(crt->raw.p, crt->raw.len);
        *flags = 0;
    } else if (crt->raw.len == s_pin_len && memcmp(crt->raw.p, s_pin, s_pin_len) == 0) {
        *flags = 0;
    } else {
        ESP_LOGE(TAG, "fremdes Zertifikat – Waage nicht verbunden");
        *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    }
    return 0;
}

static esp_err_t tls_attach(void *conf)
{
    if (!s_chain_loaded) {
        mbedtls_x509_crt_init(&s_chain);
        if (mbedtls_x509_crt_parse(&s_chain, setup_cert_start, setup_cert_end - setup_cert_start) != 0) {
            return ESP_FAIL;
        }
        s_chain_loaded = true;
    }
    load_pin();
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(conf, &s_chain, NULL);
    mbedtls_ssl_conf_verify(conf, verify_cb, NULL);
    return ESP_OK;
}

void kenwood_scale_forget(void)
{
    free(s_pin);
    s_pin = NULL;
    s_pin_len = 0;
    s_pin_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// WebSocket (nur das Nötigste: Text empfangen, Ping beantworten)

// 0 = ok, -1 = Fehler/Stille, -2 = nicht mehr gebraucht
static int read_exact(esp_tls_t *tls, uint8_t *p, size_t n, int64_t *last_rx)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = esp_tls_conn_read(tls, p + got, n - got);
        int64_t now = esp_timer_get_time();
        if (r > 0) {
            got += r;
            *last_rx = now;
            continue;
        }
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!needed()) return -2;
            if (now - *last_rx > SILENCE_US) return -1;
            continue;
        }
        return -1;  // 0 = Gegenseite hat geschlossen, < 0 = Fehler
    }
    return 0;
}

static bool write_all(esp_tls_t *tls, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = esp_tls_conn_write(tls, p, n);
        if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

// Client-Frames müssen maskiert sein (RFC 6455). Nur kurze Steuer-Frames (≤ 125 Byte).
static bool send_frame(esp_tls_t *tls, uint8_t opcode, const uint8_t *data, size_t len)
{
    if (len > 125) return false;
    uint8_t f[2 + 4 + 125];
    f[0] = 0x80 | opcode;
    f[1] = 0x80 | (uint8_t)len;
    esp_fill_random(f + 2, 4);
    for (size_t i = 0; i < len; i++) f[6 + i] = data[i] ^ f[2 + (i & 3)];
    return write_all(tls, f, 6 + len);
}

static bool handshake(esp_tls_t *tls, const char *host)
{
    // Der Server prüft den Schlüssel nicht gegen; ein fester Wert genügt (Beispiel aus RFC 6455).
    char req[192];
    int n = snprintf(req, sizeof(req),
                     "GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     host);
    if (!write_all(tls, (const uint8_t *)req, n)) return false;

    // Antwort-Kopf bis zur Leerzeile lesen (byteweise, er ist kurz)
    char head[256];
    size_t len = 0;
    int64_t last = esp_timer_get_time();
    while (len < sizeof(head) - 1) {
        if (read_exact(tls, (uint8_t *)head + len, 1, &last) != 0) return false;
        len++;
        if (len >= 4 && memcmp(head + len - 4, "\r\n\r\n", 4) == 0) break;
    }
    head[len] = '\0';
    if (!strstr(head, " 101")) {
        ESP_LOGW(TAG, "kein WebSocket: %.60s", head);
        return false;
    }
    return true;
}

static void handle_text(char *msg)
{
    if (!strstr(msg, "\"weightUpdate\"")) {
        // andere Ereignisse sind selten – zum Kennenlernen ins Log
        ESP_LOGI(TAG, "Ereignis: %.200s", msg);
        return;
    }
    const char *unit = strstr(msg, "\"unit\"");
    if (unit && !strstr(unit, "gram")) return;
    const char *v = strstr(msg, "\"value\"");
    if (!v || !(v = strchr(v, ':'))) return;
    char *end;
    float g = strtof(v + 1, &end);
    if (end != v + 1) appliance_report_weight(g);
}

static void session(uint32_t ip)
{
    char host[16];
    struct in_addr a = {.s_addr = ip};
    inet_ntoa_r(a, host, sizeof(host));

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = tls_attach,
        .skip_common_name = true,
        .timeout_ms = 5000,
    };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return;
    if (esp_tls_conn_new_sync(host, strlen(host), PORT, &cfg, tls) != 1) {
        ESP_LOGW(TAG, "keine Verbindung zu %s:%d", host, PORT);
        esp_tls_conn_destroy(tls);
        return;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0) {
        struct timeval tv = {.tv_sec = RCV_TIMEOUT_S};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (!handshake(tls, host)) {
        esp_tls_conn_destroy(tls);
        return;
    }
    ESP_LOGI(TAG, "verbunden mit %s", host);

    uint8_t *buf = malloc(FRAME_MAX + 1);
    int64_t last = esp_timer_get_time();
    bool first = true;
    while (buf) {
        // Die Maschine sendet ohne Pause (~20/s) – das Ende der Lease hier prüfen, nicht nur bei Stille.
        if (!needed()) break;
        uint8_t h[2];
        if (read_exact(tls, h, 2, &last) != 0) break;
        uint8_t op = h[0] & 0x0f;
        bool masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if (read_exact(tls, e, 2, &last) != 0) break;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (read_exact(tls, e, 8, &last) != 0) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        }
        uint8_t mask[4] = {0};
        if (masked && read_exact(tls, mask, 4, &last) != 0) break;

        if (len > FRAME_MAX) {  // zu groß: in Stücken verwerfen
            bool ok = true;
            while (len && ok) {
                size_t k = len > FRAME_MAX ? FRAME_MAX : (size_t)len;
                ok = read_exact(tls, buf, k, &last) == 0;
                len -= k;
            }
            if (!ok) break;
            continue;
        }
        if (len && read_exact(tls, buf, (size_t)len, &last) != 0) break;
        if (masked) {
            for (size_t i = 0; i < len; i++) buf[i] ^= mask[i & 3];
        }
        buf[len] = '\0';

        if (op == 0x1 && (h[0] & 0x80)) {
            if (first) {
                ESP_LOGI(TAG, "erste Nachricht: %.200s", (char *)buf);
                first = false;
            }
            handle_text((char *)buf);
        } else if (op == 0x9) {
            if (!send_frame(tls, 0xA, buf, (size_t)len)) break;  // Pong mit gleichem Inhalt
        } else if (op == 0x8) {
            ESP_LOGI(TAG, "Maschine hat die Waage geschlossen");
            break;
        }
    }
    if (buf) send_frame(tls, 0x8, NULL, 0);  // höflich schließen (Fehler egal)
    free(buf);
    esp_tls_conn_destroy(tls);
    ESP_LOGI(TAG, "getrennt");
}

static void scale_task(void *arg)
{
    ESP_LOGI(TAG, "an");
    while (true) {
        if (needed()) {
            appliance_status_t st;
            appliance_get_status(&st);
            uint32_t ip = s_host;
            if (st.connected && ip) {
                session(ip);
            } else {
                ESP_LOGD(TAG, "warte: Maschine %s, Adresse %s", st.connected ? "da" : "fehlt", ip ? "bekannt" : "unbekannt");
            }
            if (needed()) vTaskDelay(pdMS_TO_TICKS(st.connected && ip ? RETRY_MS : 1000));
            continue;
        }
        portENTER_CRITICAL(&s_mux);
        bool still = needed();
        if (!still) s_running = false;
        portEXIT_CRITICAL(&s_mux);
        if (!still) break;
    }
    appliance_scale_stopped();
    ESP_LOGI(TAG, "aus");
    vTaskDelete(NULL);
}

void kenwood_scale_set_host(uint32_t ip) { s_host = ip; }

esp_err_t kenwood_scale_watch(uint32_t seconds)
{
    int64_t until = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    bool start = false;
    portENTER_CRITICAL(&s_mux);
    if (until > s_until_us) s_until_us = until;
    if (!s_running) s_running = start = true;
    portEXIT_CRITICAL(&s_mux);
    if (!start) return ESP_OK;

    // TLS-Handshake braucht Stack (RSA/ECDHE in mbedTLS)
    if (xTaskCreate(scale_task, "waage", 6144, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "kein RAM fuer den Waage-Task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
