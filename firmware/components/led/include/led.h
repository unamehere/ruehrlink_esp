// led – RGB-Status-LED der Brücke (WS2812 an einem GPIO)
//
// Im Normalbetrieb aus, zeigt nur Ereignisse (Entscheidung 12.11.):
//   blau blinkt   Kopplung eines Handys läuft
//   gelb blinkt   Einrichtung / Kopplung der Maschine läuft
//   grün          kurz: erfolgreich
//   rot           kurz blinkend: Fehler
#pragma once

#include <stdint.h>

typedef enum {
    LED_OFF,
    LED_PAIRING,    // bleibt, bis ein anderes Signal kommt
    LED_SETUP,      // bleibt, bis ein anderes Signal kommt
    LED_OK,         // 2 s, dann aus
    LED_ERROR,      // 3× blinken, dann aus
} led_signal_t;

void led_init(void);
void led_signal(led_signal_t s);
void led_set_brightness(uint8_t level);  // 0 = aus … 255
