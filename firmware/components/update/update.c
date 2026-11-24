#include "update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "signing_pubkey.h"

static const char *TAG = "update";

// So lange darf die neue Firmware brauchen, bis sich eine App meldet (WLAN + API + Token laufen).
#define CONFIRM_TIMEOUT_US (5LL * 60 * 1000 * 1000)

static bool s_pending;  // Probebetrieb nach einem Update
static esp_timer_handle_t s_rollback_timer;

static const esp_partition_t *s_part;
static esp_ota_handle_t s_ota;
static psa_hash_operation_t s_hash;
static size_t s_size, s_written;

// ---------------------------------------------------------------------------
// Probebetrieb und Rollback

static void rollback_cb(void *arg)
{
    ESP_LOGE(TAG, "Keine App hat sich gemeldet – zurueck zur vorherigen Firmware");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void update_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    ESP_LOGI(TAG, "Firmware %s aus %s", esp_app_get_description()->version, run->label);
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    s_pending = true;
    ESP_LOGW(TAG, "Neue Firmware im Probebetrieb – warte auf die App (max. 5 min)");
    const esp_timer_create_args_t a = {.callback = rollback_cb, .name = "ota_rollback"};
    if (esp_timer_create(&a, &s_rollback_timer) == ESP_OK) esp_timer_start_once(s_rollback_timer, CONFIRM_TIMEOUT_US);
}

void update_confirm(void)
{
    if (!s_pending) return;
    s_pending = false;
    if (s_rollback_timer) esp_timer_stop(s_rollback_timer);
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Neue Firmware bestaetigt");
}

// ---------------------------------------------------------------------------
// Empfangen

esp_err_t update_begin(size_t size)
{
    if (s_part) return ESP_ERR_INVALID_STATE;
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p) return ESP_ERR_NOT_FOUND;
    if (size == 0 || size > p->size) return ESP_ERR_INVALID_SIZE;
    // Stückweise löschen, sonst blockiert das Löschen von 3 MB vor dem ersten Byte mehrere Sekunden.
    esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if (err != ESP_OK) return err;
    s_hash = psa_hash_operation_init();
    if (psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(s_ota);
        return ESP_FAIL;
    }
    s_part = p;
    s_size = size;
    s_written = 0;
    ESP_LOGI(TAG, "Update: %u Byte nach %s", (unsigned)size, p->label);
    return ESP_OK;
}

esp_err_t update_write(const void *data, size_t len)
{
    if (!s_part) return ESP_ERR_INVALID_STATE;
    if (s_written + len > s_size) return ESP_ERR_INVALID_SIZE;
    // Image für einen anderen Chip (z. B. S3-Firmware auf der C6-Platine) gleich am Anfang ablehnen;
    // esp_ota_end würde es auch ablehnen, aber erst nach dem ganzen Upload.
    if (s_written == 0 && len >= sizeof(esp_image_header_t)) {
        const esp_image_header_t *h = data;
        if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            ESP_LOGE(TAG, "Image fuer Chip-ID %d, dieser Chip hat %d", h->chip_id, CONFIG_IDF_FIRMWARE_CHIP_ID);
            return ESP_ERR_INVALID_VERSION;
        }
    }
    esp_err_t err = esp_ota_write(s_ota, data, len);
    if (err != ESP_OK) return err;
    if (psa_hash_update(&s_hash, data, len) != PSA_SUCCESS) return ESP_FAIL;
    s_written += len;
    return ESP_OK;
}

void update_abort(void)
{
    if (!s_part) return;
    esp_ota_abort(s_ota);
    psa_hash_abort(&s_hash);
    s_part = NULL;
    ESP_LOGW(TAG, "Update abgebrochen");
}

static bool signature_ok(const uint8_t hash[32], const uint8_t sig[UPDATE_SIG_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t key;
    if (psa_import_key(&attr, SIGNING_PUBKEY, sizeof(SIGNING_PUBKEY), &key) != PSA_SUCCESS) return false;
    psa_status_t st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, sig, UPDATE_SIG_LEN);
    psa_destroy_key(key);
    return st == PSA_SUCCESS;
}

esp_err_t update_finish(const uint8_t sha256[32], const uint8_t sig[UPDATE_SIG_LEN], const char **code)
{
    *code = "image";
    if (!s_part) return ESP_ERR_INVALID_STATE;
    uint8_t hash[32];
    size_t hlen = 0;
    bool hashed = psa_hash_finish(&s_hash, hash, sizeof(hash), &hlen) == PSA_SUCCESS;
    const esp_partition_t *p = s_part;

    esp_err_t err = ESP_FAIL;
    if (s_written != s_size) {
        *code = "size";
    } else if (!hashed || memcmp(hash, sha256, sizeof(hash)) != 0) {
        *code = "sha256";
    } else if (!signature_ok(hash, sig)) {
        *code = "signature";
    } else {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        esp_ota_abort(s_ota);
        s_part = NULL;
        ESP_LOGE(TAG, "Update verworfen: %s", *code);
        return ESP_ERR_INVALID_ARG;
    }

    s_part = NULL;
    err = esp_ota_end(s_ota);  // prüft das Image (Aufbau, Prüfsumme des Images)
    esp_app_desc_t desc;
    if (err == ESP_OK) err = esp_ota_get_partition_description(p, &desc);
    if (err == ESP_OK && strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        err = ESP_ERR_INVALID_VERSION;  // signiert, aber nicht für die Brücke
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image ungueltig: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Update auf %s bereit (%s), Neustart folgt", desc.version, p->label);
    *code = NULL;
    return ESP_OK;
}

static void restart_cb(void *arg) { esp_restart(); }

void update_restart_later(int ms)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "ota_restart"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK) esp_restart();
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}
