// update – Firmware-Update über die App (OTA in die inaktive Partition, signiert, mit Rollback)
//
// Ablauf und Format: docs/updates.md. Es läuft immer höchstens ein Update gleichzeitig.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define UPDATE_SIG_LEN 64  // ECDSA P-256, r||s

// Beim Start aufrufen. Läuft eine frisch aktualisierte Firmware im Probebetrieb, beginnt die Frist für den
// Selbsttest; meldet sich bis dahin keine App (update_confirm), geht es zurück auf die alte Firmware.
void update_init(void);

// Die App hat sich erfolgreich gemeldet → neue Firmware gilt als gut. Billig, darf bei jeder Anfrage kommen.
void update_confirm(void);

// Neues Image empfangen: begin → write … → finish (oder abort).
esp_err_t update_begin(size_t size);
esp_err_t update_write(const void *data, size_t len);

// Prüft Größe, SHA-256, Signatur und Image; setzt die neue Partition als Startpartition.
// Bei Fehler: *code = "size" | "sha256" | "signature" | "image".
esp_err_t update_finish(const uint8_t sha256[32], const uint8_t sig[UPDATE_SIG_LEN], const char **code);
void update_abort(void);

// Neustart nach `ms` (damit die HTTP-Antwort noch rausgeht).
void update_restart_later(int ms);
