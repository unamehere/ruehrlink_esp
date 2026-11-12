// appliance – geräteneutrale Schnittstelle zur Küchenmaschine
//
// Ablaufsteuerung, API und App kennen nur dieses Modul. Ein Gerätetreiber (z. B. `kenwood`)
// meldet sich mit appliance_register() an, liefert seine Fähigkeiten und meldet Status und
// Bestätigungen zurück. Jede Statusänderung wird als Ereignis APPLIANCE_EVENT verteilt.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

// Was das Gerät kann – die App baut ihre Regler daraus.
typedef struct {
    const char *model;      // z. B. "kenwood-kcl95"
    const char *name;       // Anzeigename, z. B. "Cooking Chef XL"
    int16_t temp_min;       // °C, kleinste einstellbare Heiztemperatur (0 = Heizung aus)
    int16_t temp_max;       // °C
    uint8_t speed_max;      // höchste Stufe (0 = aus)
    uint32_t time_max_s;    // längste Programmzeit
    bool has_scale;         // Waage vorhanden
    bool remote_start;      // Start aus der Ferne möglich (Kenwood: nein, Knopf drücken)
} appliance_caps_t;

// Ein Programmschritt: so wird er an das Gerät gesendet.
typedef struct {
    int16_t temperature;    // °C, 0 = nicht heizen
    uint32_t time_s;
    uint8_t speed;
} appliance_program_t;

// Gemeinsamer Status aller Geräte. Gerätespezifisches bleibt im Treiber.
typedef struct {
    bool connected;         // Gerät mit der Brücke verbunden
    bool running;           // Motor/Programm läuft
    bool heating;
    bool hot;
    bool bowl_fitted;
    bool lid_closed;        // Kopf/Deckel verriegelt
    int16_t bowl_temp;      // °C
    int16_t temp_set;       // °C
    uint8_t speed_set;
    uint32_t remaining_s;
} appliance_status_t;

// Treiber-Ops. Alle Funktionen kehren sofort zurück; Ergebnisse kommen als Ereignis.
typedef struct {
    const appliance_caps_t *caps;
    esp_err_t (*start)(void);                                  // Server/Verbindung starten
    esp_err_t (*send_program)(const appliance_program_t *p);   // Werte senden
    esp_err_t (*stop)(void);                                   // laufendes Programm beenden
    esp_err_t (*onboard)(void);                                // Gerät mit der Brücke koppeln
} appliance_driver_t;

ESP_EVENT_DECLARE_BASE(APPLIANCE_EVENT);
typedef enum {
    APPLIANCE_EVENT_STATUS,     // Daten: appliance_status_t
    APPLIANCE_EVENT_ACK,        // Daten: bool (Programm übernommen ja/nein)
    APPLIANCE_EVENT_WEIGHT,     // Daten: float (Gramm)
    APPLIANCE_EVENT_ONBOARD,    // Daten: appliance_onboard_t
} appliance_event_t;

typedef enum {
    APPLIANCE_ONBOARD_SEARCHING,    // suche das Setup-WLAN des Geräts
    APPLIANCE_ONBOARD_SENT,         // Zugangsdaten übergeben, warte auf Verbindung
    APPLIANCE_ONBOARD_DONE,
    APPLIANCE_ONBOARD_FAILED,
} appliance_onboard_t;

// --- für Ablaufsteuerung und API -------------------------------------------

const appliance_caps_t *appliance_caps(void);
void appliance_get_status(appliance_status_t *out);

// Prüft die Werte gegen die Fähigkeiten und sendet sie. ESP_ERR_INVALID_ARG bei
// Werten außerhalb der Grenzen, ESP_ERR_INVALID_STATE wenn kein Gerät verbunden ist.
esp_err_t appliance_send(const appliance_program_t *p);
esp_err_t appliance_stop(void);
esp_err_t appliance_onboard(void);

// --- für Treiber ----------------------------------------------------------

esp_err_t appliance_register(const appliance_driver_t *driver);  // startet den Treiber
void appliance_set_status(const appliance_status_t *s);           // Ereignis nur bei Änderung
void appliance_report_ack(bool ok);
void appliance_report_weight(float grams);
void appliance_report_onboard(appliance_onboard_t state);
