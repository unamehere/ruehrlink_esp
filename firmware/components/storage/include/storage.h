// storage – Dateispeicher der Brücke (Rezepte, Fotos, Programme, Logs)
//
// Alles liegt als normale Dateien unter STORAGE_ROOT und wird mit fopen/fread/...
// gelesen. Beim Start wird die microSD (SPI, Pins in menuconfig "Ruehrlink: Speicher")
// eingehängt; ohne lesbare Karte die SD-Emulation im Flash. Für den Rest unsichtbar.
//
// Einstellungen und Geheimnisse (WLAN, Token, AP-Passwort) gehören NICHT hierher,
// sondern in NVS – die SD-Karte kann man herausnehmen und auslesen.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_ROOT "/data"

// Dateisystem einhängen, bei Bedarf formatieren. Einmal beim Start aufrufen.
esp_err_t storage_init(void);

// Was gerade eingehängt ist: "sd", "flash" (Rückfall/Emulation) oder "none".
const char *storage_backend(void);

esp_err_t storage_usage(uint64_t *total_bytes, uint64_t *free_bytes);

// Datei sicher ersetzen: erst nach <path>.tmp schreiben, dann über <path>.new umbenennen.
// Bei Stromausfall bleibt entweder die alte oder die neue Fassung erhalten.
// Dateien auf .tmp und .new gehören dem Speicher: beim Start wird .tmp gelöscht, .new übernommen.
esp_err_t storage_write(const char *path, const void *data, size_t len);

// Ganze Datei lesen (+ abschließendes '\0'). Der Aufrufer gibt den Puffer mit free() frei.
// Gibt NULL zurück, wenn die Datei fehlt oder größer als max_len ist.
char *storage_read(const char *path, size_t max_len, size_t *out_len);

esp_err_t storage_remove(const char *path);

// Verzeichnis (auch verschachtelt) anlegen, falls es fehlt.
esp_err_t storage_mkdirs(const char *path);

// Alle Nutzerdaten löschen (Rezepte, Fotos, Programme, Protokolle) – vor Weitergabe/Verkauf.
// microSD wird neu formatiert, der Flash-Speicher (Rückfall bzw. alte Kopie) komplett gelöscht.
// Danach ist kein Speicher mehr eingehängt; nur noch neu starten.
esp_err_t storage_wipe(void);
