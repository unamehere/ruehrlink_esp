#include "sequencer.h"

#include <stdlib.h>
#include <string.h>

#include "appliance.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sequencer";

ESP_EVENT_DEFINE_BASE(SEQUENCER_EVENT);

#define TICK_MS          250
#define LEAD_S           2                          // nächsten Schritt so viele Sekunden vor Ablauf senden
#define ACK_WAIT_US      (7 * 1000000LL)            // der Treiber meldet nach 5 s selbst "nicht bestätigt"
#define MAX_RETRIES      2
#define RAMP_US          (800 * 1000LL)
#define RESTART_WAIT_US  (6 * 1000000LL)            // Motor nach Ruhephase wieder an?
#define PAUSE_MAX_US     (30LL * 60 * 1000000)
#define FALLBACK_US      (10 * 1000000LL)           // falls die Restzeit der Maschine nicht kommt
#define MAX_PLAN         16                         // Rampe + Zielwert

typedef struct {
    char program_id[32];
    char name[40];
    sequencer_step_t steps[SEQUENCER_MAX_STEPS];
    int n;
} job_t;

typedef enum { MSG_START, MSG_ABORT, MSG_STATUS, MSG_ACK } msg_type_t;

typedef struct {
    msg_type_t type;
    union {
        job_t *job;
        appliance_status_t st;
        bool ok;
    };
} msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static sequencer_info_t s_info;     // geteilt, nur unter s_lock

// Alles Folgende gehört dem Task.
static job_t s_job;
static int s_idx;
static appliance_status_t s_st;
static appliance_program_t s_plan[MAX_PLAN];
static int s_plan_n, s_plan_pos, s_retries;
static bool s_await_ack, s_need_start, s_seen_counting, s_restart_pending;
static int64_t s_ack_deadline, s_next_send_at, s_step_started, s_elapsed_us, s_last_tick, s_paused_since,
    s_restart_deadline, s_last_pub;
static sequencer_state_t s_state = SEQUENCER_IDLE;
static sequencer_reason_t s_reason = SEQUENCER_REASON_NONE;

// ---------------------------------------------------------------------------
// Hilfen

const char *sequencer_state_name(sequencer_state_t s)
{
    static const char *n[] = {"idle", "waiting_start", "running", "paused", "done", "aborted"};
    return s <= SEQUENCER_ABORTED ? n[s] : "idle";
}

const char *sequencer_reason_name(sequencer_reason_t r)
{
    static const char *n[] = {NULL,           "press_start", "lid_open", "bowl_missing", "machine_paused",
                              "disconnected", "no_ack",      "user",     "timeout"};
    return r <= SEQUENCER_REASON_TIMEOUT ? n[r] : NULL;
}

static bool is_active(sequencer_state_t s)
{
    return s == SEQUENCER_WAITING_START || s == SEQUENCER_RUNNING || s == SEQUENCER_PAUSED;
}

static int speed_index(const appliance_caps_t *c, uint8_t v)
{
    for (int i = 0; i < c->n_speeds; i++) {
        if (c->speeds[i].v == v) return i;
    }
    return -1;
}

static bool continuous(const appliance_caps_t *c, uint8_t v)
{
    int i = speed_index(c, v);
    return i >= 0 && c->speeds[i].group == APPLIANCE_SPEED_CONTINUOUS;
}

// Dauerstufe über der Grenze für heißen Inhalt?
static bool too_fast_when_hot(const appliance_caps_t *c, uint8_t v)
{
    return continuous(c, v) && speed_index(c, v) > speed_index(c, c->hot_speed_max);
}

static bool is_timer_step(const sequencer_step_t *s) { return s->speed == 0; }

static bool started(const appliance_status_t *st) { return st->running || st->heating; }

static void publish(int64_t now)
{
    sequencer_info_t info;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = s_state;
    s_info.reason = s_reason;
    if (s_job.n > 0 && s_idx < s_job.n) {
        const sequencer_step_t *step = &s_job.steps[s_idx];
        s_info.step = s_idx + 1;
        s_info.n_steps = s_job.n;
        strlcpy(s_info.label, step->label, sizeof(s_info.label));
        if (is_timer_step(step)) {
            int64_t left = (int64_t)step->time_s * 1000000 - s_elapsed_us;
            s_info.step_remaining_s = left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
        } else {
            s_info.step_remaining_s = s_st.remaining_s;
        }
    }
    info = s_info;
    xSemaphoreGive(s_lock);
    s_last_pub = now;
    esp_event_post(SEQUENCER_EVENT, SEQUENCER_EVENT_CHANGED, &info, sizeof(info), 0);
}

static void set_state(sequencer_state_t st, sequencer_reason_t r, int64_t now)
{
    if (st != s_state || r != s_reason) {
        ESP_LOGI(TAG, "Zustand: %s%s%s (Schritt %d/%d)", sequencer_state_name(st), r ? " – " : "",
                 r ? sequencer_reason_name(r) : "", s_idx + 1, s_job.n);
    }
    if (st == SEQUENCER_RUNNING && s_state != SEQUENCER_RUNNING) {
        // Uhr des Schritts neu starten: Warte- und Pausenzeit zählt nicht (nur für die Rückfall-Zeit)
        s_step_started = now;
        s_seen_counting = false;
    }
    s_state = st;
    s_reason = r;
    publish(now);
}

// ---------------------------------------------------------------------------
// Senden mit Bestätigung, Wiederholung und Rampe

static void clear_plan(void)
{
    s_plan_n = s_plan_pos = s_retries = 0;
    s_await_ack = false;
}

static void abort_seq(sequencer_reason_t r, int64_t now)
{
    clear_plan();
    s_restart_pending = false;
    if (r != SEQUENCER_REASON_DISCONNECTED) appliance_stop();
    set_state(SEQUENCER_ABORTED, r, now);
}

// Schritt (bzw. Restzeit davon) als Plan anlegen: Zwischenstufen der Rampe, dann der Zielwert.
static void plan_step(const sequencer_step_t *step, uint32_t time_s, bool ramp, int64_t now)
{
    const appliance_caps_t *c = appliance_caps();
    appliance_program_t target = {.temperature = step->temperature, .time_s = time_s, .speed = step->speed};
    if (s_st.bowl_temp >= c->hot_temp && too_fast_when_hot(c, target.speed)) {
        ESP_LOGW(TAG, "Inhalt heiss (%d C): Stufe %u auf %u begrenzt", s_st.bowl_temp, target.speed, c->hot_speed_max);
        target.speed = c->hot_speed_max;
    }
    clear_plan();
    if (ramp && s_st.running && continuous(c, s_st.speed_set) && continuous(c, target.speed)) {
        for (int i = speed_index(c, s_st.speed_set) + 1; i < speed_index(c, target.speed) && s_plan_n < MAX_PLAN - 1; i++) {
            s_plan[s_plan_n] = target;
            s_plan[s_plan_n++].speed = c->speeds[i].v;
        }
    }
    s_plan[s_plan_n++] = target;
    s_next_send_at = now;
}

static void send_planned(int64_t now)
{
    const appliance_program_t *p = &s_plan[s_plan_pos];
    esp_err_t err = appliance_send(p);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Senden fehlgeschlagen: %s", esp_err_to_name(err));
        abort_seq(err == ESP_ERR_INVALID_STATE ? SEQUENCER_REASON_DISCONNECTED : SEQUENCER_REASON_NO_ACK, now);
        return;
    }
    ESP_LOGI(TAG, "-> Schritt %d: %d C, %lu s, Stufe %u%s", s_idx + 1, p->temperature, (unsigned long)p->time_s,
             p->speed, s_plan_pos + 1 < s_plan_n ? " (Rampe)" : "");
    s_await_ack = true;
    s_ack_deadline = now + ACK_WAIT_US;
}

// Schritt ist bei der Maschine angekommen.
static void step_sent(int64_t now)
{
    s_step_started = now;
    s_seen_counting = false;
    if (s_need_start) {
        s_need_start = false;
        if (started(&s_st)) set_state(SEQUENCER_RUNNING, SEQUENCER_REASON_NONE, now);
        else set_state(SEQUENCER_WAITING_START, SEQUENCER_REASON_PRESS_START, now);
    } else {
        publish(now);
    }
}

static void on_ack(bool ok, int64_t now)
{
    if (!s_await_ack) return;  // z. B. Bestätigung des Stopps
    s_await_ack = false;
    if (!ok) {
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "keine Bestaetigung – Wiederholung %d", s_retries);
            s_next_send_at = now;
        } else {
            abort_seq(SEQUENCER_REASON_NO_ACK, now);
        }
        return;
    }
    s_retries = 0;
    ESP_LOGI(TAG, "<- bestaetigt");
    if (++s_plan_pos < s_plan_n) {
        s_next_send_at = now + RAMP_US;
    } else {
        clear_plan();
        step_sent(now);
    }
}

// ---------------------------------------------------------------------------
// Ablauf

static void begin_step(int idx, int64_t now)
{
    const sequencer_step_t *prev = idx > 0 ? &s_job.steps[idx - 1] : NULL;
    s_idx = idx;
    s_elapsed_us = 0;
    const sequencer_step_t *step = &s_job.steps[idx];
    // nach einer Ruhephase muss der Motor wieder anlaufen – ohne Knopfdruck? (an der Maschine zu testen)
    s_restart_pending = prev && is_timer_step(prev) && !is_timer_step(step) && !s_need_start;
    s_restart_deadline = now + RESTART_WAIT_US;
    plan_step(step, step->time_s, true, now);
    publish(now);
}

static void finish(int64_t now)
{
    if (is_timer_step(&s_job.steps[s_idx])) appliance_stop();  // Heizen am Ende abschalten
    set_state(SEQUENCER_DONE, SEQUENCER_REASON_NONE, now);
}

static void step_end(int64_t now)
{
    if (s_idx + 1 < s_job.n) begin_step(s_idx + 1, now);
    else finish(now);
}

static void pause_seq(sequencer_reason_t r, int64_t now)
{
    clear_plan();
    s_restart_pending = false;
    s_paused_since = now;
    set_state(SEQUENCER_PAUSED, r, now);
}

// Nach Schüssel/Kopf: aktuellen Schritt mit der Restzeit erneut senden, dann auf Knopfdruck warten.
static void resume_after_safety(int64_t now)
{
    const sequencer_step_t *step = &s_job.steps[s_idx];
    uint32_t left;
    if (is_timer_step(step)) {
        int64_t l = (int64_t)step->time_s * 1000000 - s_elapsed_us;
        left = l > 0 ? (uint32_t)(l / 1000000) + 1 : 1;
    } else {
        left = s_st.remaining_s > 0 ? s_st.remaining_s : step->time_s;
    }
    s_need_start = true;
    plan_step(step, left, false, now);
    set_state(SEQUENCER_WAITING_START, SEQUENCER_REASON_PRESS_START, now);
}

static void start_job(job_t *job, int64_t now)
{
    s_job = *job;
    free(job);
    clear_plan();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_info, 0, sizeof(s_info));
    strlcpy(s_info.program_id, s_job.program_id, sizeof(s_info.program_id));
    strlcpy(s_info.name, s_job.name, sizeof(s_info.name));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Programm \"%s\" (%s) mit %d Schritten", s_job.name, s_job.program_id[0] ? s_job.program_id : "inline",
             s_job.n);
    s_need_start = true;
    s_state = SEQUENCER_WAITING_START;
    s_reason = SEQUENCER_REASON_NONE;
    begin_step(0, now);
}

static void on_status(const appliance_status_t *st, int64_t now)
{
    appliance_status_t prev = s_st;
    s_st = *st;
    if (!is_active(s_state)) return;

    if (!st->connected) {
        abort_seq(SEQUENCER_REASON_DISCONNECTED, now);
        return;
    }
    bool safe = st->bowl_fitted && st->lid_closed;
    if (!safe && s_state != SEQUENCER_PAUSED) {
        pause_seq(st->bowl_fitted ? SEQUENCER_REASON_LID_OPEN : SEQUENCER_REASON_BOWL_MISSING, now);
        return;
    }
    if (s_state == SEQUENCER_PAUSED) {
        if (safe && (s_reason == SEQUENCER_REASON_LID_OPEN || s_reason == SEQUENCER_REASON_BOWL_MISSING)) {
            resume_after_safety(now);
        } else if (s_reason == SEQUENCER_REASON_MACHINE_PAUSED && started(st)) {
            set_state(SEQUENCER_RUNNING, SEQUENCER_REASON_NONE, now);
        } else if (!safe && s_reason != SEQUENCER_REASON_LID_OPEN && s_reason != SEQUENCER_REASON_BOWL_MISSING) {
            pause_seq(st->bowl_fitted ? SEQUENCER_REASON_LID_OPEN : SEQUENCER_REASON_BOWL_MISSING, now);
        }
        return;
    }
    if (s_state == SEQUENCER_WAITING_START) {
        if (started(st) && s_plan_n == 0) set_state(SEQUENCER_RUNNING, SEQUENCER_REASON_NONE, now);
        return;
    }
    // RUNNING: am Gerät angehalten? (nicht kurz vor Schrittende und nicht nach einer Ruhephase)
    const sequencer_step_t *step = &s_job.steps[s_idx];
    if (!is_timer_step(step) && !s_restart_pending && s_plan_n == 0 && prev.running && !st->running &&
        st->remaining_s > LEAD_S) {
        pause_seq(SEQUENCER_REASON_MACHINE_PAUSED, now);
    }
}

static void tick(int64_t now)
{
    int64_t dt = s_last_tick ? now - s_last_tick : 0;
    s_last_tick = now;
    if (!is_active(s_state)) return;

    // Plan abarbeiten (Rampe, Wiederholungen)
    if (s_plan_n > 0) {
        if (!s_await_ack && now >= s_next_send_at) send_planned(now);
        else if (s_await_ack && now > s_ack_deadline) on_ack(false, now);
        return;
    }
    if (s_state == SEQUENCER_PAUSED) {
        if (now - s_paused_since > PAUSE_MAX_US) abort_seq(SEQUENCER_REASON_TIMEOUT, now);
        return;
    }
    if (s_state != SEQUENCER_RUNNING) return;

    const sequencer_step_t *step = &s_job.steps[s_idx];
    bool last = s_idx + 1 >= s_job.n;
    if (is_timer_step(step)) {
        s_elapsed_us += dt;
        if (s_elapsed_us >= (int64_t)step->time_s * 1000000) step_end(now);
        else if (now - s_last_pub >= 1000000) publish(now);
        return;
    }
    if (s_restart_pending) {
        if (s_st.running) {
            s_restart_pending = false;
            s_step_started = now;
        } else if (now > s_restart_deadline) {
            s_restart_pending = false;
            s_need_start = false;
            set_state(SEQUENCER_WAITING_START, SEQUENCER_REASON_PRESS_START, now);
        }
        return;
    }
    if (s_st.remaining_s > LEAD_S) s_seen_counting = true;
    bool overdue = now - s_step_started > (int64_t)step->time_s * 1000000 + FALLBACK_US;
    if (last) {
        if ((s_seen_counting && !s_st.running && s_st.remaining_s == 0) || overdue) finish(now);
    } else if ((s_seen_counting && s_st.remaining_s <= LEAD_S) || overdue) {
        step_end(now);
    }
}

static void task(void *arg)
{
    msg_t m;
    for (;;) {
        bool got = xQueueReceive(s_queue, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        if (got) {
            switch (m.type) {
            case MSG_START: start_job(m.job, now); break;
            case MSG_ABORT:
                if (is_active(s_state)) abort_seq(SEQUENCER_REASON_USER, now);
                break;
            case MSG_STATUS: on_status(&m.st, now); break;
            case MSG_ACK: on_ack(m.ok, now); break;
            }
        }
        tick(now);
    }
}

static void on_appliance_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    msg_t m;
    if (id == APPLIANCE_EVENT_STATUS) {
        m.type = MSG_STATUS;
        m.st = *(appliance_status_t *)data;
    } else if (id == APPLIANCE_EVENT_ACK) {
        m.type = MSG_ACK;
        m.ok = *(bool *)data;
    } else {
        return;
    }
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) ESP_LOGW(TAG, "Warteschlange voll");
}

// ---------------------------------------------------------------------------
// Öffentlich

static esp_err_t validate(const sequencer_step_t *steps, int n, sequencer_error_t *err)
{
    const appliance_caps_t *c = appliance_caps();
    err->step = -1;
    if (!c) return (err->code = "not_connected"), ESP_ERR_INVALID_STATE;
    if (n <= 0) return (err->code = "empty"), ESP_ERR_INVALID_ARG;
    if (n > SEQUENCER_MAX_STEPS) return (err->code = "too_many"), ESP_ERR_INVALID_ARG;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const sequencer_step_t *s = &steps[i];
        err->step = i;
        bool temp_ok = s->temperature == 0 || (s->temperature >= c->temp_min && s->temperature <= c->temp_max);
        if (!temp_ok || speed_index(c, s->speed) < 0 || s->time_s == 0 || s->time_s > c->time_max_s) {
            return (err->code = "range"), ESP_ERR_INVALID_ARG;
        }
        if (s->temperature >= c->hot_temp && too_fast_when_hot(c, s->speed)) {
            return (err->code = "hot_speed"), ESP_ERR_INVALID_ARG;
        }
        total += s->time_s;
    }
    err->step = -1;
    if (total > c->time_max_s) return (err->code = "total_time"), ESP_ERR_INVALID_ARG;
    if (steps[0].speed == 0 && steps[0].temperature == 0) {
        err->step = 0;
        return (err->code = "first_step"), ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t sequencer_start(const char *program_id, const char *name, const sequencer_step_t *steps, int n,
                          sequencer_error_t *err)
{
    esp_err_t e = validate(steps, n, err);
    if (e != ESP_OK) return e;
    appliance_status_t st;
    appliance_get_status(&st);
    if (!st.connected) return (err->code = "not_connected"), ESP_ERR_INVALID_STATE;
    if (!st.bowl_fitted || !st.lid_closed) return (err->code = "not_ready"), ESP_ERR_INVALID_STATE;

    job_t *job = calloc(1, sizeof(job_t));
    if (!job) return ESP_ERR_NO_MEM;
    strlcpy(job->program_id, program_id ? program_id : "", sizeof(job->program_id));
    strlcpy(job->name, name ? name : "", sizeof(job->name));
    memcpy(job->steps, steps, n * sizeof(sequencer_step_t));
    job->n = n;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = is_active(s_info.state);
    if (!busy) s_info.state = SEQUENCER_WAITING_START;  // reserviert, der Task füllt den Rest
    xSemaphoreGive(s_lock);
    if (busy) {
        free(job);
        return (err->code = "busy"), ESP_ERR_INVALID_STATE;
    }
    msg_t m = {.type = MSG_START, .job = job};
    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(job);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.state = SEQUENCER_IDLE;
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t sequencer_abort(void)
{
    if (!sequencer_active()) return ESP_ERR_INVALID_STATE;
    msg_t m = {.type = MSG_ABORT};
    return xQueueSend(s_queue, &m, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool sequencer_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool a = is_active(s_info.state);
    xSemaphoreGive(s_lock);
    return a;
}

void sequencer_get(sequencer_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
}

esp_err_t sequencer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(16, sizeof(msg_t));
    if (!s_lock || !s_queue) return ESP_ERR_NO_MEM;
    appliance_get_status(&s_st);
    if (xTaskCreate(task, "sequencer", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return esp_event_handler_register(APPLIANCE_EVENT, ESP_EVENT_ANY_ID, on_appliance_event, NULL);
}
