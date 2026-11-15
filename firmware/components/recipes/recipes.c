#include "recipes.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "psa/crypto.h"

#include "storage.h"

static const char *TAG = "recipes";

#define RECIPE_DIR STORAGE_ROOT "/recipes"
#define MEDIA_DIR  STORAGE_ROOT "/media"
#define PATH_LEN   96

// So viel RAM muss nach dem Einlesen eines Rezepts noch frei sein (WLAN, TLS zur Maschine, Sockets).
// Der cJSON-Baum braucht grob das Doppelte des Texts.
#define HEAP_RESERVE (24 * 1024)
// So viel Platz bleibt auf dem Speicher frei, damit Rezepte (klein) immer noch passen.
#define MEDIA_RESERVE (256 * 1024)

static uint32_t s_rev;

// ---------------------------------------------------------------------------
// Dateinamen: <id>.<rev>.json | <id>.<rev>.del

bool recipes_valid_id(const char *id)
{
    size_t n = strlen(id);
    return n > 0 && n <= RECIPES_ID_MAX && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") == n;
}

static bool parse_name(const char *name, recipes_entry_t *e)
{
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) return false;
    if (strcasecmp(ext, ".json") == 0) e->deleted = false;
    else if (strcasecmp(ext, ".del") == 0) e->deleted = true;
    else return false;  // .tmp/.new des Speichers, Fremdes

    const char *dot = ext - 1;
    while (dot > name && *dot != '.') dot--;
    size_t id_len = (size_t)(dot - name), rev_len = (size_t)(ext - dot - 1);
    if (dot == name || id_len > RECIPES_ID_MAX || rev_len == 0 || rev_len > 9) return false;
    uint32_t rev = 0;
    for (const char *p = dot + 1; p < ext; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        rev = rev * 10 + (uint32_t)(*p - '0');
    }
    // FAT liefert ohne langen Namen evtl. Großbuchstaben
    for (size_t i = 0; i < id_len; i++) e->id[i] = (char)tolower((unsigned char)name[i]);
    e->id[id_len] = '\0';
    e->rev = rev;
    return rev > 0 && recipes_valid_id(e->id);
}

void recipes_path(const recipes_entry_t *e, char *path, size_t len)
{
    snprintf(path, len, RECIPE_DIR "/%s.%" PRIu32 ".%s", e->id, e->rev, e->deleted ? "del" : "json");
}

static void remove_entry(const recipes_entry_t *e)
{
    char path[PATH_LEN];
    recipes_path(e, path, sizeof(path));
    storage_remove(path);
}

esp_err_t recipes_foreach(bool (*fn)(const recipes_entry_t *e, void *ctx), void *ctx)
{
    DIR *d = opendir(RECIPE_DIR);
    if (!d) return ESP_FAIL;
    struct dirent *de;
    recipes_entry_t e;
    while ((de = readdir(d)) != NULL) {
        if (parse_name(de->d_name, &e) && !fn(&e, ctx)) break;
    }
    closedir(d);
    return ESP_OK;
}

// Neuesten Eintrag zu id suchen. Ältere Fassungen (nur nach einem Stromausfall zwischen Schreiben
// und Löschen möglich) werden dabei entfernt – erst nach dem Durchlauf, nicht im offenen Verzeichnis.
typedef struct {
    const char *id;
    recipes_entry_t best;
    recipes_entry_t stale[4];
    int n_stale;
} find_ctx_t;

static bool find_visit(const recipes_entry_t *e, void *arg)
{
    find_ctx_t *c = arg;
    if (strcmp(e->id, c->id) != 0) return true;
    const recipes_entry_t *old = e;
    if (e->rev > c->best.rev) {
        old = c->best.rev ? &c->best : NULL;
        if (old && c->n_stale < 4) c->stale[c->n_stale++] = *old;
        c->best = *e;
        return true;
    }
    if (c->n_stale < 4) c->stale[c->n_stale++] = *old;
    return true;
}

esp_err_t recipes_find(const char *id, recipes_entry_t *out)
{
    if (!recipes_valid_id(id)) return ESP_ERR_NOT_FOUND;
    find_ctx_t c = {.id = id};
    if (recipes_foreach(find_visit, &c) != ESP_OK) return ESP_FAIL;
    for (int i = 0; i < c.n_stale; i++) {
        ESP_LOGW(TAG, "alte Fassung entfernt: %s rev %" PRIu32, c.stale[i].id, c.stale[i].rev);
        remove_entry(&c.stale[i]);
    }
    if (!c.best.rev) return ESP_ERR_NOT_FOUND;
    if (out) *out = c.best;
    return ESP_OK;
}

uint32_t recipes_rev(void) { return s_rev; }

// ---------------------------------------------------------------------------
// Start: höchste Revision bestimmen, doppelte Fassungen eines Rezepts aufräumen

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    uint32_t *hashes;
    size_t n, cap;
    bool oom;
} scan_ctx_t;

static bool scan_visit(const recipes_entry_t *e, void *arg)
{
    scan_ctx_t *c = arg;
    if (e->rev > s_rev) s_rev = e->rev;
    if (c->oom) return true;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        uint32_t *h = realloc(c->hashes, cap * sizeof(uint32_t));
        if (!h) {
            c->oom = true;
            return true;
        }
        c->hashes = h;
        c->cap = cap;
    }
    c->hashes[c->n++] = fnv1a(e->id);
    return true;
}

typedef struct {
    const uint32_t *dups;
    size_t n_dups;
    char ids[4][RECIPES_ID_MAX + 1];
    int n_ids;
} dup_ctx_t;

static bool dup_visit(const recipes_entry_t *e, void *arg)
{
    dup_ctx_t *c = arg;
    uint32_t h = fnv1a(e->id);
    if (!bsearch(&h, c->dups, c->n_dups, sizeof(h), cmp_u32)) return true;
    for (int i = 0; i < c->n_ids; i++) {
        if (strcmp(c->ids[i], e->id) == 0) return true;
    }
    if (c->n_ids == 4) return false;
    strcpy(c->ids[c->n_ids++], e->id);
    return true;
}

esp_err_t recipes_init(void)
{
    if (storage_mkdirs(RECIPE_DIR) != ESP_OK || storage_mkdirs(MEDIA_DIR) != ESP_OK) {
        ESP_LOGE(TAG, "Verzeichnisse fehlen");
        return ESP_FAIL;
    }
    scan_ctx_t c = {0};
    recipes_foreach(scan_visit, &c);

    // Gleiche Hashes = (vermutlich) zwei Fassungen desselben Rezepts. Kommt nur nach einem
    // Stromausfall mitten im Speichern vor, daher reicht es, ein paar davon aufzuräumen.
    size_t n_dups = 0;
    if (c.n > 1) {
        qsort(c.hashes, c.n, sizeof(uint32_t), cmp_u32);
        for (size_t i = 1; i < c.n; i++) {
            if (c.hashes[i] == c.hashes[i - 1] && (n_dups == 0 || c.hashes[n_dups - 1] != c.hashes[i])) {
                c.hashes[n_dups++] = c.hashes[i];  // vorne im selben Feld sammeln
            }
        }
    }
    if (n_dups) {
        dup_ctx_t d = {.dups = c.hashes, .n_dups = n_dups};
        recipes_foreach(dup_visit, &d);
        for (int i = 0; i < d.n_ids; i++) recipes_find(d.ids[i], NULL);
    }
    free(c.hashes);
    ESP_LOGI(TAG, "%u Eintraege, Revision %" PRIu32, (unsigned)c.n, s_rev);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Rezept speichern

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

static bool absent(const cJSON *v) { return v == NULL || cJSON_IsNull(v); }

static bool array_of(const cJSON *a, int type, int max)
{
    if (absent(a)) return true;
    if (!cJSON_IsArray(a)) return false;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, a) {
        if ((e->type & 0xFF) != type || ++n > max) return false;
    }
    return true;
}

static bool photo_ok(const cJSON *p) { return absent(p) || (cJSON_IsString(p) && recipes_media_valid(p->valuestring)); }

// Nur so viel prüfen, dass die App sicher damit arbeiten kann. Unbekannte Felder bleiben erhalten
// (neuere App), Maschinenwerte prüft der Sequencer beim Start.
static const char *validate(const cJSON *j)
{
    if (!cJSON_IsObject(j)) return "json";
    const cJSON *t = get(j, "title");
    if (!cJSON_IsString(t) || !t->valuestring[0] || strlen(t->valuestring) > 200) return "title";
    const cJSON *s = get(j, "servings");
    if (!absent(s) && !(cJSON_IsNumber(s) && s->valuedouble >= 1 && s->valuedouble <= 1000)) return "servings";
    if (!photo_ok(get(j, "photo"))) return "photo";
    if (!array_of(get(j, "tags"), cJSON_String, 50)) return "tags";
    if (!array_of(get(j, "ingredients"), cJSON_Object, 200)) return "ingredients";
    const cJSON *steps = get(j, "steps");
    if (!array_of(steps, cJSON_Object, 200)) return "steps";
    const cJSON *st;
    cJSON_ArrayForEach(st, steps) {
        const cJSON *txt = get(st, "text"), *w = get(st, "weigh"), *m = get(st, "machine");
        const cJSON *tool = get(st, "tool");
        if (!absent(txt) && !cJSON_IsString(txt)) return "steps";
        if (!photo_ok(get(st, "photo"))) return "steps";
        // Zutaten pro Schritt (seit App 0.3.3); `weigh` nur noch in alten Rezepten
        if (!array_of(get(st, "ingredients"), cJSON_Object, 50)) return "steps";
        if (!absent(tool) && !(cJSON_IsString(tool) && strlen(tool->valuestring) <= 32)) return "steps";
        if (!absent(w) && !(cJSON_IsObject(w) && cJSON_IsNumber(get(w, "target")))) return "steps";
        if (!absent(m) && !cJSON_IsObject(m)) return "steps";
    }
    return NULL;
}

// Feld vorne einfügen (vorhandenes gleichen Namens ersetzen) – die Datei beginnt dann mit v, id, rev, updated.
static void set_front(cJSON *j, const char *key, cJSON *item)
{
    cJSON_DeleteItemFromObjectCaseSensitive(j, key);
    if (!cJSON_AddItemToObject(j, key, item)) {
        cJSON_Delete(item);
        return;
    }
    cJSON_InsertItemInArray(j, 0, cJSON_DetachItemViaPointer(j, item));
}

// cJSON liest rekursiv: tief verschachteltes JSON würde den Stack des HTTP-Servers sprengen.
// Ein Rezept braucht 5 Ebenen (Rezept → steps → Schritt → ingredients → Zutat); vor dem Parsen grob zählen.
#define MAX_DEPTH 8

static bool depth_ok(const char *s, size_t len)
{
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') i++;
            else if (c == '"') in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > MAX_DEPTH) return false;
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return true;
}

static void now_iso(char *buf, size_t len)
{
    buf[0] = '\0';
    time_t now = time(NULL);
    if (now < 1700000000) return;  // Uhr noch nicht von der App gestellt
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

esp_err_t recipes_put(const char *id, char *body, size_t len, size_t cap, int64_t if_match, recipes_result_t *res)
{
    memset(res, 0, sizeof(*res));
    esp_err_t err = ESP_OK;
    cJSON *j = NULL;
    recipes_entry_t cur;
    bool have = false;

    if (!recipes_valid_id(id)) {
        res->error = "id";
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }
    if (len > RECIPES_MAX_JSON || cap < len + RECIPES_JSON_SLACK) {
        res->error = "size";
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    have = recipes_find(id, &cur) == ESP_OK;
    uint32_t live = have && !cur.deleted ? cur.rev : 0;
    if (if_match >= 0 && (uint64_t)if_match != live) {
        res->rev = live;
        res->deleted = have && cur.deleted;
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 2 * len + HEAP_RESERVE) {
        ESP_LOGW(TAG, "zu wenig RAM fuer %u Byte", (unsigned)len);
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    j = depth_ok(body, len) ? cJSON_ParseWithLength(body, len) : NULL;
    if (!j) {
        res->error = "json";
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }
    res->error = validate(j);
    if (res->error) {
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }

    uint32_t rev = s_rev + 1;
    now_iso(res->updated, sizeof(res->updated));
    if (!res->updated[0]) {
        // Ohne gestellte Uhr die Zeit der App übernehmen
        const cJSON *u = get(j, "updated");
        if (cJSON_IsString(u) && strlen(u->valuestring) < sizeof(res->updated)) {
            strcpy(res->updated, u->valuestring);
        }
    }
    set_front(j, "updated", cJSON_CreateString(res->updated));
    set_front(j, "rev", cJSON_CreateNumber(rev));
    set_front(j, "id", cJSON_CreateString(id));
    set_front(j, "v", cJSON_CreateNumber(1));

    // Ausgabe in den Eingabepuffer – der wird nach dem Parsen nicht mehr gebraucht.
    if (!cJSON_PrintPreallocated(j, body, (int)cap, false)) {
        res->error = "size";
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    cJSON_Delete(j);
    j = NULL;

    char path[PATH_LEN];
    recipes_entry_t next = {.rev = rev, .deleted = false};
    strcpy(next.id, id);
    recipes_path(&next, path, sizeof(path));
    if (storage_write(path, body, strlen(body)) != ESP_OK) {
        err = ESP_FAIL;
        goto out;
    }
    s_rev = rev;
    if (have) remove_entry(&cur);  // alte Fassung bzw. Tombstone
    res->rev = rev;
    ESP_LOGI(TAG, "%s gespeichert, rev %" PRIu32, id, rev);

out:
    cJSON_Delete(j);
    free(body);
    return err;
}

esp_err_t recipes_delete(const char *id, int64_t if_match, recipes_result_t *res)
{
    memset(res, 0, sizeof(*res));
    recipes_entry_t cur;
    if (recipes_find(id, &cur) != ESP_OK || cur.deleted) return ESP_ERR_NOT_FOUND;
    if (if_match >= 0 && (uint64_t)if_match != cur.rev) {
        res->rev = cur.rev;
        return ESP_ERR_INVALID_STATE;
    }
    recipes_entry_t tomb = cur;
    tomb.rev = s_rev + 1;
    tomb.deleted = true;
    char path[PATH_LEN];
    recipes_path(&tomb, path, sizeof(path));
    if (storage_write(path, "", 0) != ESP_OK) return ESP_FAIL;
    s_rev = tomb.rev;
    remove_entry(&cur);
    res->rev = tomb.rev;
    res->deleted = true;
    ESP_LOGI(TAG, "%s geloescht, rev %" PRIu32, id, tomb.rev);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Fotos

bool recipes_media_valid(const char *sha_hex)
{
    return strlen(sha_hex) == RECIPES_SHA_HEX && strspn(sha_hex, "0123456789abcdef") == RECIPES_SHA_HEX;
}

void recipes_media_path(const char *sha_hex, char *path, size_t len)
{
    snprintf(path, len, MEDIA_DIR "/%s.webp", sha_hex);
}

struct recipes_upload {
    FILE *f;
    psa_hash_operation_t hash;
    size_t size, got;
    uint8_t head[12];
    char sha[RECIPES_SHA_HEX + 1];
    char tmp[PATH_LEN + 8];
};

esp_err_t recipes_media_begin(const char *sha_hex, size_t size, recipes_upload_t **out)
{
    *out = NULL;
    if (!recipes_media_valid(sha_hex)) return ESP_ERR_INVALID_ARG;
    if (size == 0 || size > RECIPES_MAX_MEDIA) return ESP_ERR_INVALID_SIZE;
    uint64_t total = 0, free_b = 0;
    if (storage_usage(&total, &free_b) != ESP_OK || free_b < size + MEDIA_RESERVE) return ESP_ERR_NO_MEM;

    recipes_upload_t *u = calloc(1, sizeof(*u));
    if (!u) return ESP_ERR_NO_MEM;
    strcpy(u->sha, sha_hex);
    u->size = size;
    // .tmp: bleibt nach einem Stromausfall nur als Rest liegen und wird beim Start gelöscht
    char final_path[PATH_LEN];
    recipes_media_path(sha_hex, final_path, sizeof(final_path));
    snprintf(u->tmp, sizeof(u->tmp), "%s.tmp", final_path);
    u->hash = psa_hash_operation_init();
    if (psa_hash_setup(&u->hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(u);
        return ESP_FAIL;
    }
    u->f = fopen(u->tmp, "wb");
    if (!u->f) {
        ESP_LOGE(TAG, "%s: %s", u->tmp, strerror(errno));
        psa_hash_abort(&u->hash);
        free(u);
        return ESP_FAIL;
    }
    *out = u;
    return ESP_OK;
}

esp_err_t recipes_media_write(recipes_upload_t *u, const void *data, size_t n)
{
    if (u->got + n > u->size) return ESP_ERR_INVALID_SIZE;
    if (u->got < sizeof(u->head)) {
        size_t k = sizeof(u->head) - u->got;
        memcpy(u->head + u->got, data, n < k ? n : k);
    }
    if (fwrite(data, 1, n, u->f) != n) return ESP_FAIL;
    if (psa_hash_update(&u->hash, data, n) != PSA_SUCCESS) return ESP_FAIL;
    u->got += n;
    return ESP_OK;
}

void recipes_media_abort(recipes_upload_t *u)
{
    if (!u) return;
    if (u->f) fclose(u->f);
    unlink(u->tmp);
    psa_hash_abort(&u->hash);
    free(u);
}

esp_err_t recipes_media_finish(recipes_upload_t *u, const char **code)
{
    uint8_t hash[32];
    size_t hlen = 0;
    bool hashed = psa_hash_finish(&u->hash, hash, sizeof(hash), &hlen) == PSA_SUCCESS;
    char hex[RECIPES_SHA_HEX + 1];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);

    bool ok = fflush(u->f) == 0;
    ok = (fsync(fileno(u->f)) == 0) && ok;
    ok = (fclose(u->f) == 0) && ok;
    u->f = NULL;

    *code = NULL;
    if (u->got != u->size) *code = "size";
    else if (memcmp(u->head, "RIFF", 4) != 0 || memcmp(u->head + 8, "WEBP", 4) != 0) *code = "format";
    else if (!hashed || strcmp(hex, u->sha) != 0) *code = "sha256";
    else if (!ok) *code = "storage";

    char final_path[PATH_LEN];
    recipes_media_path(u->sha, final_path, sizeof(final_path));
    if (!*code) {
        unlink(final_path);  // gleicher Inhalt; FAT benennt nicht über bestehende Dateien um
        if (rename(u->tmp, final_path) != 0) *code = "storage";
    }
    if (*code) unlink(u->tmp);
    else ESP_LOGI(TAG, "Foto %.12s... gespeichert (%u Byte)", u->sha, (unsigned)u->size);
    free(u);
    return *code ? ESP_FAIL : ESP_OK;
}
