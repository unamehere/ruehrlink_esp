// sequencer – arbeitet Programme (Schrittfolgen) auf der Brücke ab, mit den Sicherheitsregeln
// aus docs/architektur.md. Kennt nur `appliance`, nie die Kenwood direkt.
//
// Ablauf: Schritt 1 senden → warten, bis am Gerät gestartet wird (kein Fernstart) → jeden weiteren
// Schritt kurz vor Ablauf live nachschieben. Zustand wird als SEQUENCER_EVENT verteilt.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#define SEQUENCER_MAX_STEPS 20

typedef struct {
    int16_t temperature;    // °C, 0 = nicht heizen
    uint32_t time_s;
    uint8_t speed;          // Gerätewert v aus caps, 0 = Motor aus (Ruhen/nur Heizen)
    char label[32];
} sequencer_step_t;

typedef enum {
    SEQUENCER_IDLE,
    SEQUENCER_WAITING_START,    // Knopf an der Maschine drücken
    SEQUENCER_RUNNING,
    SEQUENCER_PAUSED,
    SEQUENCER_DONE,
    SEQUENCER_ABORTED,
} sequencer_state_t;

typedef enum {
    SEQUENCER_REASON_NONE,
    SEQUENCER_REASON_PRESS_START,
    SEQUENCER_REASON_LID_OPEN,
    SEQUENCER_REASON_BOWL_MISSING,
    SEQUENCER_REASON_MACHINE_PAUSED,
    SEQUENCER_REASON_DISCONNECTED,
    SEQUENCER_REASON_NO_ACK,
    SEQUENCER_REASON_USER,
    SEQUENCER_REASON_TIMEOUT,
} sequencer_reason_t;

typedef struct {
    sequencer_state_t state;
    sequencer_reason_t reason;
    char program_id[32];    // "" bei Inline-Programmen
    char name[40];
    uint8_t step;           // ab 1, 0 = kein Programm
    uint8_t n_steps;
    char label[32];
    uint32_t step_remaining_s;
} sequencer_info_t;

ESP_EVENT_DECLARE_BASE(SEQUENCER_EVENT);
typedef enum {
    SEQUENCER_EVENT_CHANGED,    // Daten: sequencer_info_t
} sequencer_event_t;

// Warum ein Start abgelehnt wurde (Codes wie in docs/api.md).
typedef struct {
    const char *code;       // "empty", "too_many", "range", "hot_speed", "total_time", "first_step",
                            // "not_connected", "busy", "not_ready"
    int step;               // betroffener Schritt (ab 0) oder -1
} sequencer_error_t;

esp_err_t sequencer_init(void);

// Prüft alle Schritte gegen die Grenzwerte und startet. ESP_ERR_INVALID_ARG bei ungültigen
// Schritten, ESP_ERR_INVALID_STATE wenn das Gerät nicht bereit ist oder schon etwas läuft.
esp_err_t sequencer_start(const char *program_id, const char *name, const sequencer_step_t *steps, int n,
                          sequencer_error_t *err);

// Bricht ab und stoppt die Maschine. ESP_ERR_INVALID_STATE, wenn nichts läuft.
esp_err_t sequencer_abort(void);

bool sequencer_active(void);    // läuft gerade ein Programm (wartend, laufend oder pausiert)?
void sequencer_get(sequencer_info_t *out);
const char *sequencer_state_name(sequencer_state_t s);
const char *sequencer_reason_name(sequencer_reason_t r);    // NULL für "kein Grund"
