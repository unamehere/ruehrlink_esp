#include "diag.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

// Achtung: In diesem Modul nie ESP_LOGx aufrufen – der Hook würde sich selbst aufrufen.

#define RING_SIZE CONFIG_RL_DIAG_LOG_SIZE
#define DIAG_LINE_MAX  192  // längere Zeilen werden gekürzt (liegt im Stack des loggenden Tasks)

static char s_ring[RING_SIZE];
static uint32_t s_head;    // nächste Schreibstelle im Puffer
static uint32_t s_total;   // insgesamt geschriebene Byte (absolute Position, läuft über)
static uint32_t s_filled;  // gültige Byte im Puffer (≤ RING_SIZE)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t s_prev;
static bool s_hooked;
static const char *volatile s_secrets[DIAG_MAX_SECRETS];

// ---------------------------------------------------------------------------
// Maskieren (alles in place, die Länge bleibt gleich bis auf entfernte Farbcodes)

// Wörter, nach denen ein Wert mit ':' oder '=' als geheim gilt ("pop":"…", token=…, Passwort: …).
static const char *const KEYS[] = {"password", "passwort", "passwd", "pass", "pwd", "psk",
                                   "pop",      "token",    "secret", "key"};

static bool is_value_end(char c)
{
    return c == '\0' || c == '"' || c == '\'' || c == ',' || c == ' ' || c == '}' || c == '&' || c == ';' ||
           c == '\n' || c == '\r';
}

static void mask_keywords(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && isalnum((unsigned char)s[i - 1])) continue;  // nur am Wortanfang
        for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
            size_t kl = strlen(KEYS[k]);
            if (i + kl > n || strncasecmp(s + i, KEYS[k], kl) != 0) continue;
            size_t j = i + kl;
            if (j < n && (s[j] == '"' || s[j] == '\'')) j++;
            while (j < n && s[j] == ' ') j++;
            if (j >= n || (s[j] != ':' && s[j] != '=')) continue;
            j++;
            while (j < n && (s[j] == ' ' || s[j] == '"' || s[j] == '\'')) j++;
            while (j < n && !is_value_end(s[j])) s[j++] = '*';
            break;
        }
    }
}

// App-Tokens sind genau 32 Hex-Zeichen (SHA-256-Namen der Fotos sind 64 und bleiben stehen).
static void mask_tokens(char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (!isxdigit((unsigned char)s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && isxdigit((unsigned char)s[j])) j++;
        bool word = (i == 0 || !isalnum((unsigned char)s[i - 1])) && (j == n || !isalnum((unsigned char)s[j]));
        if (word && j - i == 32) memset(s + i, '*', 32);
        i = j;
    }
}

static void mask_secrets(char *s, size_t n)
{
    for (int k = 0; k < DIAG_MAX_SECRETS; k++) {
        const char *sec = s_secrets[k];
        if (!sec) continue;
        size_t sl = strnlen(sec, 128);
        if (sl < 6 || sl > n) continue;
        for (size_t i = 0; i + sl <= n; i++) {
            if (memcmp(s + i, sec, sl) == 0) {
                memset(s + i, '*', sl);
                i += sl - 1;
            }
        }
    }
}

// Farbcodes der Konsole (ESC [ … m) entfernen. Liefert die neue Länge.
static size_t strip_ansi(char *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\x1b' && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && !isalpha((unsigned char)s[i])) i++;
            continue;
        }
        s[o++] = s[i];
    }
    return o;
}

// Gekürzte Zeile nicht mitten in einem UTF-8-Zeichen enden lassen.
static size_t utf8_trim(const char *s, size_t n)
{
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return n;
    unsigned char lead = (unsigned char)s[i - 1];
    if (lead < 0xC0) return n;  // ASCII am Ende
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    return (n - (i - 1) < need) ? i - 1 : n;
}

// ---------------------------------------------------------------------------
// Ringpuffer

static void ring_put(const char *s, size_t n)
{
    if (n > RING_SIZE) {
        s += n - RING_SIZE;
        n = RING_SIZE;
    }
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t first = RING_SIZE - s_head;
    if (first > n) first = n;
    memcpy(s_ring + s_head, s, first);
    memcpy(s_ring, s + first, n - first);
    s_head = (s_head + n) % RING_SIZE;
    s_total += n;
    s_filled = s_filled + n > RING_SIZE ? RING_SIZE : s_filled + n;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

static int log_hook(const char *fmt, va_list args)
{
    char line[DIAG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n;
        bool cut = len >= sizeof(line);  // gekürzt: Platz für den Zeilenumbruch lassen
        if (cut) len = utf8_trim(line, sizeof(line) - 2);
        line[len] = '\0';
        len = strip_ansi(line, len);
        line[len] = '\0';
        mask_secrets(line, len);
        mask_keywords(line, len);
        mask_tokens(line, len);
        if (cut && len > 0 && line[len - 1] != '\n') line[len++] = '\n';
        ring_put(line, len);
    }
    return s_prev ? s_prev(fmt, args) : vprintf(fmt, args);
}

// ---------------------------------------------------------------------------

void diag_init(void)
{
    if (s_hooked) return;
    s_hooked = true;
    s_prev = esp_log_set_vprintf(log_hook);
}

void diag_add_secret(const char *secret)
{
    if (!secret) return;
    for (int k = 0; k < DIAG_MAX_SECRETS; k++) {
        if (s_secrets[k] == secret) return;
        if (!s_secrets[k]) {
            s_secrets[k] = secret;
            return;
        }
    }
}

void diag_log_span(uint32_t *first, uint32_t *end)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    *end = s_total;
    *first = s_total - s_filled;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

size_t diag_log_read(uint32_t *pos, uint32_t end, char *out, size_t len)
{
    size_t n = 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    uint32_t first = s_total - s_filled;
    if ((int32_t)(*pos - first) < 0) *pos = first;  // inzwischen überschrieben
    int32_t avail = (int32_t)(end - *pos);
    if (avail > 0) {
        n = (size_t)avail < len ? (size_t)avail : len;
        uint32_t back = s_total - *pos;  // so weit liegt *pos hinter der Schreibstelle
        size_t idx = (s_head + RING_SIZE - back) % RING_SIZE;
        size_t part = RING_SIZE - idx;
        if (part > n) part = n;
        memcpy(out, s_ring + idx, part);
        memcpy(out + part, s_ring, n - part);
        *pos += n;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return n;
}
