#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "storage";

#define PATH_MAX_LEN 128

// ---------------------------------------------------------------------------
// Backends – nur dieser Abschnitt unterscheidet SD und Flash.
//
// Reihenfolge beim Start: microSD (SPI) → falls keine/unlesbare Karte: SD-Emulation im Flash.
// Liegen Daten im Flash und steckt eine leere Karte, werden sie einmal auf die Karte kopiert
// (copy_flash_to_sd). Der Flash bleibt dabei unverändert – nur eine Markierung kommt dazu.

#define FLASH_PARTITION "storage"
#define FLASH_MOUNT     "/flash"            // nur während des Kopierens eingehängt
#define COPIED_MARK     "/.auf-sd-kopiert"  // im Flash: schon einmal auf eine Karte kopiert
#define IMPORT_DIR      "/.flash-import"    // auf der Karte: Kopie läuft (nach Abbruch verworfen)

static const char *s_backend = "none";

#if CONFIG_RL_STORAGE_FLASH_FALLBACK
#include "esp_partition.h"
#include "wear_levelling.h"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

static esp_err_t flash_mount(const char *base, bool format, wl_handle_t *wl)
{
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, FLASH_PARTITION)) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = format,
        .max_files = CONFIG_RL_STORAGE_MAX_FILES,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(base, FLASH_PARTITION, &cfg, wl);
}
#endif

#if CONFIG_RL_STORAGE_USE_SD
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *s_card;

#if CONFIG_RL_SD_FORMAT_IF_FAILED
#define SD_FORMAT true
#else
#define SD_FORMAT false
#endif

static esp_err_t sd_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = CONFIG_RL_SD_FREQ_KHZ;
    const spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_RL_SD_PIN_MOSI,
        .miso_io_num = CONFIG_RL_SD_PIN_MISO,
        .sclk_io_num = CONFIG_RL_SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,  // klein halten: DMA-Puffer liegen im internen RAM
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) return err;
    // Interne Pull-ups als Reserve – SD-Module haben meist eigene, die Platine hat 10 kΩ.
    gpio_pullup_en(CONFIG_RL_SD_PIN_MISO);
    gpio_pullup_en(CONFIG_RL_SD_PIN_CS);

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = CONFIG_RL_SD_PIN_CS;
    dev.host_id = SPI2_HOST;
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = SD_FORMAT,
        .max_files = CONFIG_RL_STORAGE_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(STORAGE_ROOT, &host, &dev, &cfg, &s_card);
    if (err != ESP_OK) {
        spi_bus_free(SPI2_HOST);  // Pins wieder frei, kein Restverbrauch ohne Karte
        s_card = NULL;
        return err;
    }
    ESP_LOGI(TAG, "microSD: %s, %llu MB, SPI %d kHz", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
             CONFIG_RL_SD_FREQ_KHZ);
    return ESP_OK;
}
#endif

const char *storage_backend(void) { return s_backend; }

// --- einmaliges Kopieren Flash → leere Karte ----------------------------------------------

#if CONFIG_RL_STORAGE_USE_SD && CONFIG_RL_STORAGE_FLASH_FALLBACK

#define COPY_BLOCK 1024

// Verzeichnis ohne eigene Dateien? Versteckte Einträge (".…") und Windows-Systemordner zählen nicht.
static bool dir_is_empty(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return true;
    bool empty = true;
    struct dirent *e;
    while (empty && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strcasecmp(e->d_name, "System Volume Information") == 0) continue;
        empty = false;
    }
    closedir(d);
    return empty;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    char *child = malloc(PATH_MAX_LEN + 8);
    struct dirent *e;
    while (child && (e = readdir(d)) != NULL) {
        if (snprintf(child, PATH_MAX_LEN + 8, "%s/%s", path, e->d_name) >= PATH_MAX_LEN + 8) continue;
        if (e->d_type == DT_DIR) remove_tree(child);
        else unlink(child);
    }
    free(child);
    closedir(d);
    rmdir(path);
}

static bool copy_file(const char *from, const char *to, char *buf)
{
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    bool ok = out != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, COPY_BLOCK, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = !ferror(in) && ok;
    fclose(in);
    if (out) ok = (fclose(out) == 0) && ok;
    return ok;
}

// Rekursiv kopieren, versteckte Einträge (Markierung, .tmp-Reste) und .tmp-Dateien auslassen.
static bool copy_tree(const char *from, const char *to, char *buf, unsigned *files)
{
    if (mkdir(to, 0775) != 0 && errno != EEXIST) return false;
    DIR *d = opendir(from);
    if (!d) return false;
    char *src = malloc(2 * (PATH_MAX_LEN + 8));
    char *dst = src ? src + PATH_MAX_LEN + 8 : NULL;
    bool ok = src != NULL;
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || (n > 4 && strcasecmp(e->d_name + n - 4, ".tmp") == 0)) continue;
        if (snprintf(src, PATH_MAX_LEN + 8, "%s/%s", from, e->d_name) >= PATH_MAX_LEN + 8 ||
            snprintf(dst, PATH_MAX_LEN + 8, "%s/%s", to, e->d_name) >= PATH_MAX_LEN + 8) {
            ok = false;
            break;
        }
        if (e->d_type == DT_DIR) {
            ok = copy_tree(src, dst, buf, files);
        } else {
            ok = copy_file(src, dst, buf);
            if (ok) (*files)++;
            else ESP_LOGE(TAG, "Kopieren fehlgeschlagen: %s", src);
        }
    }
    free(src);
    closedir(d);
    return ok;
}

// Steckt eine leere Karte und liegen im Flash Daten, die noch nie kopiert wurden: einmal kopieren.
// Erst in einen versteckten Ordner, dann umbenennen – bricht der Strom ab, bleibt die Karte "leer"
// und der nächste Start versucht es noch einmal.
static void copy_flash_to_sd(void)
{
    remove_tree(STORAGE_ROOT IMPORT_DIR);  // Rest eines abgebrochenen Versuchs
    if (!dir_is_empty(STORAGE_ROOT)) return;

    wl_handle_t wl = WL_INVALID_HANDLE;
    if (flash_mount(FLASH_MOUNT, false, &wl) != ESP_OK) return;  // nie formatieren, nur lesen
    struct stat st;
    char *buf = NULL;
    if (stat(FLASH_MOUNT COPIED_MARK, &st) == 0 || dir_is_empty(FLASH_MOUNT) || !(buf = malloc(COPY_BLOCK))) {
        goto out;
    }
    ESP_LOGW(TAG, "leere microSD: kopiere die Daten aus dem Flash (einmalig) ...");
    unsigned files = 0;
    bool ok = copy_tree(FLASH_MOUNT, STORAGE_ROOT IMPORT_DIR, buf, &files);
    if (ok) {
        // Inhalt des Import-Ordners an seinen Platz – je Eintrag neu lesen, nicht während des Umbenennens
        char *a = malloc(2 * (PATH_MAX_LEN + 8));
        char *b = a ? a + PATH_MAX_LEN + 8 : NULL;
        ok = a != NULL;
        while (ok) {
            DIR *d = opendir(STORAGE_ROOT IMPORT_DIR);
            struct dirent *e = d ? readdir(d) : NULL;
            bool fits = e && snprintf(a, PATH_MAX_LEN + 8, STORAGE_ROOT IMPORT_DIR "/%s", e->d_name) < PATH_MAX_LEN + 8 &&
                        snprintf(b, PATH_MAX_LEN + 8, STORAGE_ROOT "/%s", e->d_name) < PATH_MAX_LEN + 8;
            if (d) closedir(d);
            if (!e) break;
            ok = fits && rename(a, b) == 0;
        }
        free(a);
    }
    remove_tree(STORAGE_ROOT IMPORT_DIR);
    if (ok) {
        FILE *f = fopen(FLASH_MOUNT COPIED_MARK, "wb");  // nicht noch einmal kopieren
        if (f) fclose(f);
        ESP_LOGW(TAG, "%u Dateien aus dem Flash auf die microSD kopiert (Flash bleibt unveraendert)", files);
    } else {
        ESP_LOGE(TAG, "Kopieren aus dem Flash fehlgeschlagen - die Karte bleibt leer, Flash unveraendert");
    }
out:
    free(buf);
    esp_vfs_fat_spiflash_unmount_rw_wl(FLASH_MOUNT, wl);
}
#endif

static esp_err_t backend_mount(void)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_RL_STORAGE_USE_SD
    err = sd_mount();
    if (err == ESP_OK) {
        s_backend = "sd";
#if CONFIG_RL_STORAGE_FLASH_FALLBACK
        copy_flash_to_sd();
#endif
        return ESP_OK;
    }
    ESP_LOGW(TAG, "keine lesbare microSD (%s) - Pins: MISO %d, MOSI %d, SCK %d, CS %d", esp_err_to_name(err),
             CONFIG_RL_SD_PIN_MISO, CONFIG_RL_SD_PIN_MOSI, CONFIG_RL_SD_PIN_SCK, CONFIG_RL_SD_PIN_CS);
#endif
#if CONFIG_RL_STORAGE_FLASH_FALLBACK
    ESP_LOGW(TAG, "RUECKFALL: SD-Emulation im Flash aktiv - Rezepte und Fotos liegen im internen Flash");
    err = flash_mount(STORAGE_ROOT, true, &s_wl);
    if (err == ESP_OK) s_backend = "flash";
#endif
    return err;
}

// ---------------------------------------------------------------------------

static void recover_tmp_files(const char *dir);

esp_err_t storage_init(void)
{
    esp_err_t err = backend_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "kein Speicher eingehaengt: %s", esp_err_to_name(err));
        return err;
    }
    uint64_t total = 0, free_b = 0;
    storage_usage(&total, &free_b);
    ESP_LOGI(TAG, "%s unter %s: %llu kB frei von %llu kB", storage_backend(), STORAGE_ROOT,
             (unsigned long long)(free_b / 1024), (unsigned long long)(total / 1024));
    recover_tmp_files(STORAGE_ROOT);
    return ESP_OK;
}

esp_err_t storage_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    return esp_vfs_fat_info(STORAGE_ROOT, total_bytes, free_bytes);
}

esp_err_t storage_mkdirs(const char *path)
{
    char buf[PATH_MAX_LEN];
    size_t root = strlen(STORAGE_ROOT);
    if (strlen(path) >= sizeof(buf) || strncmp(path, STORAGE_ROOT "/", root + 1) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(buf, path);
    for (char *p = buf + root + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
        *p = '/';
    }
    if (mkdir(buf, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t storage_write(const char *path, const void *data, size_t len)
{
    // Zwei Stufen, damit nach einem Stromausfall klar ist, was vollständig ist:
    //   <path>.tmp – wird gerade geschrieben (evtl. unvollständig)
    //   <path>.new – vollständig geschrieben, nur noch nicht an seinem Platz
    char tmp[PATH_MAX_LEN + 8], done[PATH_MAX_LEN + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return ESP_ERR_INVALID_ARG;
    snprintf(done, sizeof(done), "%s.new", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "%s: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    unlink(done);
    if (!ok || rename(tmp, done) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    // FAT kann nicht über eine bestehende Datei umbenennen: erst die alte löschen.
    // Fällt genau dazwischen der Strom aus, stellt recover_tmp_files() die .new wieder her.
    unlink(path);
    if (rename(done, path) != 0) {
        ESP_LOGE(TAG, "rename %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

char *storage_read(const char *path, size_t max_len, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0 || (size_t)st.st_size > max_len) return NULL;

    char *buf = malloc(st.st_size + 1);
    if (!buf) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        free(buf);
        return NULL;
    }
    size_t n = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

esp_err_t storage_remove(const char *path)
{
    return (unlink(path) == 0 || errno == ENOENT) ? ESP_OK : ESP_FAIL;
}

// Nach einem Stromausfall:
//   <x>.tmp → Schreiben wurde unterbrochen (evtl. unvollständig) → löschen, alte Fassung bleibt
//   <x>.new → war fertig geschrieben, nur nicht an seinem Platz → übernehmen (ersetzt <x>)
// Früher galt „.tmp ohne <x> = fertig“ – das stimmte nicht für neu angelegte Dateien:
// eine halb geschriebene neue Datei wäre übernommen worden.
static void recover_tmp_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[PATH_MAX_LEN + 8], final_path[PATH_MAX_LEN + 8];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        if (e->d_type == DT_DIR) {
            recover_tmp_files(path);
            continue;
        }
        size_t n = strlen(path);
        if (n < 5) continue;
        if (strcasecmp(path + n - 4, ".tmp") == 0) {
            unlink(path);
            ESP_LOGW(TAG, "unvollstaendig, verworfen: %s", path);
        } else if (strcasecmp(path + n - 4, ".new") == 0) {
            memcpy(final_path, path, n - 4);
            final_path[n - 4] = '\0';
            unlink(final_path);
            if (rename(path, final_path) == 0) ESP_LOGW(TAG, "wiederhergestellt: %s", final_path);
        }
    }
    closedir(d);
}
