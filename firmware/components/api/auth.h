// Tokens der gekoppelten Apps (intern im Modul api). Ablauf siehe docs/api.md "Koppeln & Anmelden".
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef void (*auth_request_cb_t)(const char *id, const char *name);

// on_request: wird bei einer neuen Kopplungsanfrage aufgerufen (→ WS an gekoppelte Apps).
void auth_init(auth_request_cb_t on_request);

// Prüft "Bearer <token>" bzw. einen nackten Token.
bool auth_check(const char *token);

typedef enum { AUTH_PAIR_WAITING, AUTH_PAIR_TOKEN, AUTH_PAIR_BUSY } auth_pair_result_t;

// Neue Anfrage (id leer) oder Nachfrage zu einer bestehenden (id gesetzt).
// WAITING: id_out enthält die Anfrage-ID. TOKEN: token_out enthält den neuen Token (einmalig).
// BUSY: eine andere Anfrage läuft noch (höchstens 60 s) – sie wird nicht ersetzt, damit niemand im WLAN
// eine fremde Anfrage übernehmen kann, während auf dem gekoppelten Handy „erlauben?“ steht.
auth_pair_result_t auth_pair(const char *id, const char *name, char *id_out, size_t id_len,
                             char *token_out, size_t token_len);

// Bestätigung durch eine gekoppelte App. false, wenn die Anfrage nicht (mehr) existiert.
bool auth_approve(const char *id);

// Token für die Bluetooth-Einrichtung erzeugen und speichern.
void auth_new_token(char *token_out, size_t len);
