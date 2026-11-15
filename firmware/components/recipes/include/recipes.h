// recipes – Rezeptbibliothek und Fotos auf dem Speicher der Brücke
//
// Rezepte liegen als JSON-Dateien unter STORAGE_ROOT/recipes, eine Datei pro Rezept. Revision und
// Löschung stehen im Dateinamen, damit der Abgleich (`since`) ohne Öffnen der Dateien auskommt:
//   <id>.<rev>.json   – Rezept in genau der Form, in der die App es bekommt (mit "v":1, id, rev, updated)
//   <id>.<rev>.del    – gelöscht (Tombstone, leer), damit andere Apps die Löschung mitbekommen
// Die Bibliotheks-Revision ist die höchste vergebene Revision; jede Änderung zählt sie um 1 hoch.
//
// Fotos liegen unter STORAGE_ROOT/media/<sha256>.webp und werden nur gestreamt (nie ganz im RAM).
//
// Nur aus EINEM Task benutzen (dem HTTP-Server der API) – es gibt keine Sperre.
// Vertrag: docs/api.md (Rezepte, Fotos).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RECIPES_MAX_JSON   (16 * 1024)    // größtes Rezept, wie es die App schickt
#define RECIPES_JSON_SLACK 512            // Platz für "v", id, rev, updated beim Speichern
#define RECIPES_MAX_MEDIA  (1024 * 1024)  // größtes Foto
#define RECIPES_ID_MAX     40             // a-z, 0-9 und '-' (UUID = 36 Zeichen)
#define RECIPES_SHA_HEX    64

typedef struct {
    char id[RECIPES_ID_MAX + 1];
    uint32_t rev;
    bool deleted;
} recipes_entry_t;

typedef struct {
    uint32_t rev;       // neue Revision bzw. bei Konflikt die aktuelle (0 = gibt es nicht)
    bool deleted;       // Konflikt: das Rezept wurde inzwischen gelöscht
    char updated[24];   // "2026-11-12T10:00:00Z" (leer, wenn die Uhr nicht gestellt ist)
    const char *error;  // Formatfehler: Feldname bzw. "json", "size"
} recipes_result_t;

// Verzeichnisse anlegen, Reste eines Stromausfalls aufräumen, Revision bestimmen.
// Nach storage_init() aufrufen.
esp_err_t recipes_init(void);

// Aktuelle Revision der Bibliothek (0 = leer, noch nie etwas gespeichert).
uint32_t recipes_rev(void);

bool recipes_valid_id(const char *id);

// Alle Rezepte und Tombstones, je Rezept genau ein Eintrag, ungeordnet.
// fn gibt false zurück → Abbruch.
esp_err_t recipes_foreach(bool (*fn)(const recipes_entry_t *e, void *ctx), void *ctx);

// Neuester Eintrag zu id (auch Tombstone). ESP_ERR_NOT_FOUND, wenn es nie ein Rezept gab.
esp_err_t recipes_find(const char *id, recipes_entry_t *out);

// Pfad der Datei zu einem Eintrag (zum Streamen an die App).
void recipes_path(const recipes_entry_t *e, char *path, size_t len);

// Rezept anlegen oder ersetzen. Übernimmt `body` (JSON, len Byte, Puffer cap ≥ len + RECIPES_JSON_SLACK)
// und gibt ihn mit free() frei – so liegt nie Eingabe, Baum und Ausgabe gleichzeitig im RAM.
// if_match < 0: ohne Prüfung; 0: darf es noch nicht geben (oder gelöscht); n: muss Revision n haben.
// ESP_OK · ESP_ERR_INVALID_STATE (Konflikt, res->rev/deleted) · ESP_ERR_INVALID_ARG (res->error)
// · ESP_ERR_INVALID_SIZE (zu groß) · ESP_ERR_NO_MEM (gerade zu wenig RAM) · ESP_FAIL (Speicher)
esp_err_t recipes_put(const char *id, char *body, size_t len, size_t cap, int64_t if_match, recipes_result_t *res);

// Rezept löschen (Tombstone). ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE (Konflikt) wie oben.
esp_err_t recipes_delete(const char *id, int64_t if_match, recipes_result_t *res);

// --- Fotos ---------------------------------------------------------------------

// 64 Hex-Zeichen, klein geschrieben
bool recipes_media_valid(const char *sha_hex);

// Pfad der Fotodatei; die Datei kann fehlen.
void recipes_media_path(const char *sha_hex, char *path, size_t len);

typedef struct recipes_upload recipes_upload_t;

// Upload beginnen. ESP_ERR_INVALID_SIZE (0 oder > RECIPES_MAX_MEDIA), ESP_ERR_NO_MEM (Speicher voll
// oder kein RAM), ESP_FAIL (Datei).
esp_err_t recipes_media_begin(const char *sha_hex, size_t size, recipes_upload_t **out);
esp_err_t recipes_media_write(recipes_upload_t *u, const void *data, size_t n);

// Prüft Größe, WebP-Kennung und SHA-256, dann erst wird das Foto sichtbar. Gibt u immer frei.
// Fehlercode in *code: "size", "format", "sha256", "storage".
esp_err_t recipes_media_finish(recipes_upload_t *u, const char **code);
void recipes_media_abort(recipes_upload_t *u);
