// strip – Schüsselbeleuchtung: WS2812/SK6812-Streifen an der Buchse J3 der Rührlink-Platine
//
// Daten an IO0 über einen 2N7002 (invertiert), 5 V über einen P-FET-Schalter an IO1. Ohne Farbe
// bleibt der Schalter aus – der Streifen zieht dann keinen Strom. Noch nicht an die App angebunden.
#pragma once

#include <stdint.h>

void strip_init(void);                           // Schalter aus, Treiber bereit
void strip_fill(uint8_t r, uint8_t g, uint8_t b); // alle LEDs; 0/0/0 = aus (5 V abgeschaltet)
