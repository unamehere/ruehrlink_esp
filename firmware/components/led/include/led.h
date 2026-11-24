// led – Status-LED der Brücke
//
// Zwei Bauformen (Kconfig RL_LED_TYPE):
//   - WS2812 an einem GPIO (ESP32-S3-DevKitC-1)
//   - zwei LEDs rot + grün (Rührlink-Platine mit ESP32-C6); gelb = beide, statt blau grün
//
// Im Normalbetrieb aus, zeigt Ereignisse nur kurz an und geht dann wieder aus
// (Entscheidung 12.11., nie dauerhaft an: 24.11.):
//   blau 3× schnell   Kopplung eines Handys beginnt   (Platine: grün)
//   gelb 3×           Einrichtung / Kopplung der Maschine beginnt
//   grün 1 s          erfolgreich
//   rot 3×            Fehler
// Dazwischen optional als einzige Daueranzeige: Maschine verbunden = grün, stark gedimmt (24.11.).
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
