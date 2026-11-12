// interne Hilfe des Moduls kenwood
#pragma once
#include "esp_err.h"

// Liest das Zertifikat des HTTPS-Servers unter `ip`:443 aus dem TLS-1.2-Handshake mit und gibt es
// als PEM im Log aus. Bricht danach ab, ohne verschlüsselt zu kommunizieren.
esp_err_t setup_cert_capture(const char *ip);
