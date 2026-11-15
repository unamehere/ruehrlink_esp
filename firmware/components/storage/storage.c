#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "storage";

#define PATH_MAX_LEN 128

// ---------------------------------------------------------------------------
// Backends – nur dieser Abschnitt unterscheidet Flash und SD.

#if CONFIG_RL_STORAGE_FLASH
#include "wear_levelling.h"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

static esp_err_t backend_mount(void)
{
    ESP_LOGW(TAG, "SD-Emulation im Flash aktiv (Entwicklung) - fuer die Beta auf microSD umstellen");
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_ROOT, "storage", &cfg, &s_wl);
}

const char *storage_backend(void) { return "flash"; }

#elif CONFIG_RL_STORAGE_SD

static esp_err_t backend_mount(void)
{
    // TODO(SD): esp_vfs_fat_sdmmc_mount() bzw. esp_vfs_fat_sdspi_mount(), Pins aus Kconfig
    ESP_LOGE(TAG, "microSD-Backend ist noch nicht implementiert");
    return ESP_ERR_NOT_SUPPORTED;
}

const char *storage_backend(void) { return "sd"; }

#endif

// ---------------------------------------------------------------------------

static void recover_tmp_files(const char *dir);

esp_err_t storage_init(void)
{
    esp_err_t err = backend_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Einhaengen fehlgeschlagen: %s", esp_err_to_name(err));
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
    char tmp[PATH_MAX_LEN + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "%s: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    // FAT kann nicht über eine bestehende Datei umbenennen: erst die alte löschen.
    // Fällt genau dazwischen der Strom aus, stellt recover_tmp_files() die .tmp wieder her.
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

char *storage_read(const char *path, size_t max_len, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0 || (size_t)st.st_size > max_len) return NULL;

    // in den PSRAM, damit der interne RAM für WLAN/TLS frei bleibt
    char *buf = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(st.st_size + 1);
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
//   <x>.tmp ohne <x>  → war fertig geschrieben, nur nicht umbenannt → übernehmen
//   <x>.tmp neben <x> → Schreiben wurde unterbrochen → alte Fassung behalten
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
        if (n < 5 || strcmp(path + n - 4, ".tmp") != 0) continue;
        memcpy(final_path, path, n - 4);
        final_path[n - 4] = '\0';
        struct stat st;
        if (stat(final_path, &st) == 0) {
            unlink(path);
        } else if (rename(path, final_path) == 0) {
            ESP_LOGW(TAG, "wiederhergestellt: %s", final_path);
        }
    }
    closedir(d);
}
