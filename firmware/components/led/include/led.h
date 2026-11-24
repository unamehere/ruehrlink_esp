// led – Status-LED der Brücke (rot + grün, Rührlink-Platine; gelb = beide)
//
// Im Normalbetrieb aus, zeigt Ereignisse nur kurz an und geht dann wieder aus
// (Entscheidung 12.11., nie dauerhaft an: 24.11.):
//   grün 3× schnell   Kopplung eines Handys beginnt
//   gelb 3×           Einrichtung / Kopplung der Maschine beginnt
//   grün 1 s          erfolgreich
//   rot 3×            Fehler
// Dazwischen optional als einzige Daueranzeige: Maschine verbunden = grün, stark gedimmt (24.11.).
//
// Die Schüsselbeleuchtung (WS2812-Streifen an J3) steckt in strip.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_OFF,
    LED_PAIRING,    // 3× blinken, dann aus
    LED_SETUP,      // 3× blinken, dann aus
    LED_OK,         // 1 s, dann aus
    LED_ERROR,      // 3× blinken, dann aus
} led_signal_t;

void led_init(void);
void led_signal(led_signal_t s);
void led_set_brightness(uint8_t level);  // 0 = aus … 255
// Grundzustand zwischen den Signalen: Maschine verbunden = grün, stark gedimmt (RL_LED_CONNECTED_PCT)
void led_set_connected(bool on);
