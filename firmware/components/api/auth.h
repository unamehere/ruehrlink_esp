// Tokens der gekoppelten Apps (intern im Modul api)
#pragma once

#include <stdbool.h>
#include <stddef.h>

void auth_init(void);

// Prüft "Bearer <token>" bzw. einen nackten Token.
bool auth_check(const char *token);

// Kopplung per Taste: erster Aufruf öffnet ein 60-s-Fenster, nach Tastendruck liefert der
// nächste Aufruf einmalig einen neuen Token. Rückgabe: true + token gefüllt, sonst false.
bool auth_pair(char *token_out, size_t len);
