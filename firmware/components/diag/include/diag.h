// diag – Diagnose für „Problem melden“: die letzten Log-Ausgaben im RAM-Ringpuffer
//
// Hängt sich mit esp_log_set_vprintf() vor die normale Ausgabe (die serielle Konsole bleibt unverändert)
// und legt eine Kopie jeder Zeile in einen festen Puffer (CONFIG_RL_DIAG_LOG_SIZE). Vor dem Speichern
// werden Zugangsdaten maskiert: Werte nach password/pass/pwd/psk/pop/token/secret/key (mit ':' oder '='),
// 32-stellige Hex-Folgen (App-Tokens) und alle mit diag_add_secret() gemeldeten Zeichenketten.
// Ausgelesen wird über GET /api/v1/diag (docs/api.md).
#pragma once

#include <stddef.h>
#include <stdint.h>

// So früh wie möglich in app_main aufrufen. Mehrfacher Aufruf schadet nicht.
void diag_init(void);

// Geheimnis (z. B. WLAN-Passwort, Einrichtungs-Code), das nie im Puffer landen darf. Gemerkt wird nur der
// Zeiger – der Speicher muss dauerhaft gültig bleiben (statischer Puffer); Inhalt darf sich ändern.
// Kürzere Werte als 6 Zeichen werden ignoriert. Höchstens DIAG_MAX_SECRETS Einträge.
#define DIAG_MAX_SECRETS 4
void diag_add_secret(const char *secret);

// Lesbarer Bereich als absolute Byte-Positionen [*first, *end) (zählt seit dem Start hoch).
void diag_log_span(uint32_t *first, uint32_t *end);

// Ab *pos höchstens `len` Byte (nicht über `end` hinaus) nach `out` kopieren, *pos weiterzählen.
// Wurde der Bereich ab *pos inzwischen überschrieben, springt *pos auf den ältesten noch vorhandenen Stand.
// Rückgabe: Anzahl kopierter Byte (0 = fertig).
size_t diag_log_read(uint32_t *pos, uint32_t end, char *out, size_t len);
