// kenwood – Treiber für die Kenwood Cooking Chef XL (KCL95)
//
// Die Brücke spielt für die Maschine die abgeschaltete Hersteller-Cloud:
// TLS-Server auf 443 (/devices/*, WebSocket), siehe docs/architektur.md.
// Nach außen nur über `appliance` sichtbar.
#pragma once

#include "appliance.h"

// Treiber-Beschreibung zum Anmelden mit appliance_register().
const appliance_driver_t *kenwood_driver(void);
