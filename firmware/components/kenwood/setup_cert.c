// Zertifikat des Einrichtungs-Servers der Maschine mitlesen (für das Pinning bei der Kopplung).
//
// Bei TLS 1.2 schickt der Server sein Zertifikat unverschlüsselt in der "Certificate"-Nachricht
// des Handshakes. Wir senden ein ClientHello, lesen bis zu dieser Nachricht und brechen dann ab –
// es wird nichts verschlüsselt übertragen und keine Prüfung abgeschaltet.

#include "setup_cert.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"

static const char *TAG = "kenwood";

#define MAX_HANDSHAKE 8192

static bool read_n(int s, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static size_t client_hello(uint8_t *out)
{
    static const uint8_t suites[] = {
        0xc0, 0x2f, 0xc0, 0x30, 0xc0, 0x2b, 0xc0, 0x2c, 0xc0, 0x13, 0xc0, 0x14,
        0xc0, 0x27, 0xc0, 0x23, 0x00, 0x9c, 0x00, 0x9d, 0x00, 0x2f, 0x00, 0x35, 0x00, 0x3c,
    };
    static const uint8_t ext[] = {
        0x00, 0x0a, 0x00, 0x08, 0x00, 0x06, 0x00, 0x17, 0x00, 0x18, 0x00, 0x1d,   // supported_groups
        0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,                                       // ec_point_formats
        0x00, 0x0d, 0x00, 0x0c, 0x00, 0x0a, 0x04, 0x01, 0x05, 0x01, 0x06, 0x01,   // signature_algorithms
        0x04, 0x03, 0x02, 0x01,
    };
    uint8_t *p = out + 9;  // nach Record-Header (5) + Handshake-Header (4)
    *p++ = 0x03; *p++ = 0x03;                       // TLS 1.2
    for (int i = 0; i < 32; i++) *p++ = esp_random() & 0xff;
    *p++ = 0;                                       // keine Session-ID
    *p++ = 0; *p++ = sizeof(suites);
    memcpy(p, suites, sizeof(suites)); p += sizeof(suites);
    *p++ = 1; *p++ = 0;                             // Kompression: keine
    *p++ = 0; *p++ = sizeof(ext);
    memcpy(p, ext, sizeof(ext)); p += sizeof(ext);
    size_t body = p - (out + 9);
    out[0] = 0x16; out[1] = 0x03; out[2] = 0x01;    // Handshake-Record
    out[3] = (body + 4) >> 8; out[4] = (body + 4) & 0xff;
    out[5] = 0x01;                                  // ClientHello
    out[6] = 0; out[7] = body >> 8; out[8] = body & 0xff;
    return p - out;
}

static void log_pem(const uint8_t *der, size_t len)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[65];
    size_t n = 0;
    ESP_LOGI(TAG, "-----BEGIN CERTIFICATE-----");
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = der[i] << 16 | (i + 1 < len ? der[i + 1] << 8 : 0) | (i + 2 < len ? der[i + 2] : 0);
        line[n++] = b64[v >> 18 & 63];
        line[n++] = b64[v >> 12 & 63];
        line[n++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        line[n++] = i + 2 < len ? b64[v & 63] : '=';
        if (n == 64 || i + 3 >= len) {
            line[n] = '\0';
            ESP_LOGI(TAG, "%s", line);
            n = 0;
        }
    }
    ESP_LOGI(TAG, "-----END CERTIFICATE-----");
}

esp_err_t setup_cert_capture(const char *ip)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return ESP_FAIL;
    struct timeval tv = {.tv_sec = 8};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(443), .sin_addr.s_addr = inet_addr(ip)};
    esp_err_t err = ESP_FAIL;
    uint8_t *hs = malloc(MAX_HANDSHAKE);
    uint8_t hello[256];
    if (!hs || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) goto out;
    size_t hl = client_hello(hello);
    if (send(s, hello, hl, 0) != (int)hl) goto out;

    // Handshake-Records sammeln, bis die Certificate-Nachricht (Typ 11) vollständig ist
    size_t have = 0;
    while (have < MAX_HANDSHAKE) {
        uint8_t rh[5];
        if (!read_n(s, rh, 5)) break;
        size_t rl = rh[3] << 8 | rh[4];
        if (rh[0] != 0x16 || have + rl > MAX_HANDSHAKE) {
            ESP_LOGW(TAG, "Zertifikat: unerwarteter Record %02x", rh[0]);
            break;
        }
        if (!read_n(s, hs + have, rl)) break;
        have += rl;
        for (size_t off = 0; off + 4 <= have;) {
            uint8_t type = hs[off];
            size_t ml = hs[off + 1] << 16 | hs[off + 2] << 8 | hs[off + 3];
            if (off + 4 + ml > have) break;  // Nachricht noch unvollständig
            if (type == 11 && ml > 6) {
                const uint8_t *c = hs + off + 4 + 3;          // erstes Zertifikat der Liste
                size_t cl = c[0] << 16 | c[1] << 8 | c[2];
                if (cl + 3 <= ml - 3) {
                    ESP_LOGI(TAG, "Zertifikat des Einrichtungs-Servers (%u Byte):", (unsigned)cl);
                    log_pem(c + 3, cl);
                    err = ESP_OK;
                }
                goto out;
            }
            if (type == 14) goto out;                         // ServerHelloDone ohne Zertifikat
            off += 4 + ml;
        }
    }
out:
    free(hs);
    close(s);
    if (err != ESP_OK) ESP_LOGW(TAG, "Zertifikat konnte nicht gelesen werden");
    return err;
}
