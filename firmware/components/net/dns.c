// Mini-DNS-Server für das Maschinen-WLAN: beantwortet jede A-Anfrage mit der eigenen IP.
// Die Maschine fragt fest bei 8.8.8.8 (api.getdrop.io, events-shards…) und landet so bei uns.

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "net_priv.h"

static const char *TAG = "dns";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define DNS_HDR_LEN 12

static uint32_t s_ip;  // Netzwerk-Byte-Reihenfolge

// Länge des QNAME ab `p` (inkl. abschließender 0), oder 0 bei kaputtem Paket.
static size_t qname_len(const uint8_t *p, size_t max)
{
    size_t i = 0;
    while (i < max) {
        uint8_t l = p[i];
        if (l == 0) return i + 1;
        if (l & 0xC0) return 0;  // Kompression in Anfragen: nicht erwartet
        i += l + 1;
    }
    return 0;
}

static int build_answer(uint8_t *buf, int len)
{
    if (len < DNS_HDR_LEN + 5) return -1;
    if (buf[2] & 0x80) return -1;                     // ist schon eine Antwort
    if (buf[4] != 0 || buf[5] != 1) return -1;        // genau eine Frage

    size_t qn = qname_len(buf + DNS_HDR_LEN, len - DNS_HDR_LEN);
    if (qn == 0 || DNS_HDR_LEN + qn + 4 > (size_t)len) return -1;
    const uint8_t *q = buf + DNS_HDR_LEN + qn;
    bool is_a = q[0] == 0 && q[1] == 1 && q[2] == 0 && q[3] == 1;  // Typ A, Klasse IN
    int end = DNS_HDR_LEN + qn + 4;

    buf[2] = 0x81;  // Antwort, rekursiv gewünscht
    buf[3] = 0x80;  // Rekursion verfügbar, NOERROR
    buf[6] = 0;
    buf[7] = is_a ? 1 : 0;                            // AAAA usw.: leere Antwort
    memset(buf + 8, 0, 4);                            // keine Authority/Additional
    if (!is_a) return end;

    if (end + 16 > DNS_MAX_LEN) return -1;
    uint8_t *a = buf + end;
    const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};  // Zeiger auf Name, A, IN, TTL 60
    memcpy(a, rr, sizeof(rr));
    memcpy(a + sizeof(rr), &s_ip, 4);
    return end + 16;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = s_ip,  // nur im Maschinen-WLAN antworten
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Socket/Bind fehlgeschlagen");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS laeuft");

    uint8_t buf[DNS_MAX_LEN];
    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (n <= 0) continue;
        int out = build_answer(buf, n);
        if (out > 0) sendto(sock, buf, out, 0, (struct sockaddr *)&src, sl);
    }
}

void dns_start(uint32_t ip_network_order)
{
    s_ip = ip_network_order;
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
}
