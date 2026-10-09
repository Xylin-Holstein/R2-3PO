#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Addiction.h"
#include "r2_diary.h"
#include "Log.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define ADDICTION_DB_PATH R2_HOME "/r2_addictions.db"
#define ADDICTION_TEXT_MAX 2048

static sqlite3 *addiction_db = NULL;
static pthread_mutex_t addiction_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_target(const char *s)
{
    return s && *s && strlen(s) <= 256;
}

static int ensure_db_locked(void)
{
    if (addiction_db) return 0;
    if (sqlite3_open_v2(ADDICTION_DB_PATH, &addiction_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Addiction] Cannot open %s: %s\n",
                ADDICTION_DB_PATH,
                addiction_db ? sqlite3_errmsg(addiction_db) : "unknown error");
        if (addiction_db) sqlite3_close(addiction_db);
        addiction_db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(addiction_db, 5000);
    if (sqlite3_exec(addiction_db,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS addiction_targets ("
        " target TEXT PRIMARY KEY COLLATE NOCASE,"
        " target_type TEXT NOT NULL DEFAULT 'activity',"
        " enjoyment INTEGER NOT NULL DEFAULT 50 CHECK(enjoyment BETWEEN 0 AND 100),"
        " status TEXT NOT NULL DEFAULT 'inactive',"
        " first_seen INTEGER NOT NULL,"
        " last_chosen INTEGER,"
        " updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS addiction_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " target TEXT NOT NULL COLLATE NOCASE,"
        " event_kind TEXT NOT NULL CHECK(event_kind IN ('choice','enjoyment')),"
        " enjoyment INTEGER NOT NULL CHECK(enjoyment BETWEEN 0 AND 100),"
        " source TEXT NOT NULL DEFAULT 'unknown',"
        " details TEXT NOT NULL DEFAULT '',"
        " occurred_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS addiction_events_target_time "
        " ON addiction_events(target,occurred_at);"
        "PRAGMA user_version=1;",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Addiction] Could not create schema: %s\n",
                sqlite3_errmsg(addiction_db));
        sqlite3_close(addiction_db);
        addiction_db = NULL;
        return -1;
    }
    return 0;
}

int r2_addiction_init(void)
{
    pthread_mutex_lock(&addiction_lock);
    int rc = ensure_db_locked();
    pthread_mutex_unlock(&addiction_lock);
    return rc;
}

void r2_addiction_shutdown(void)
{
    pthread_mutex_lock(&addiction_lock);
    if (addiction_db) {
        sqlite3_wal_checkpoint_v2(addiction_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(addiction_db);
        addiction_db = NULL;
    }
    pthread_mutex_unlock(&addiction_lock);
}

static int upsert_target_locked(const char *target, const char *target_type,
                                int enjoyment, sqlite3_int64 now)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(addiction_db,
        "INSERT INTO addiction_targets(target,target_type,enjoyment,status,first_seen,last_chosen,updated_at)"
        " VALUES(?,?,?,'inactive',?,NULL,?)"
        " ON CONFLICT(target) DO UPDATE SET target_type=excluded.target_type,"
        " enjoyment=excluded.enjoyment,updated_at=excluded.updated_at",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, target_type && *target_type ? target_type : "activity", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, enjoyment);
        sqlite3_bind_int64(st, 4, now);
        sqlite3_bind_int64(st, 5, now);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int update_status_locked(const char *target, const char *target_type,
                                int *changed, char *old_status, size_t old_cap,
                                char *new_status, size_t new_cap,
                                int *choice_count_out, int *distinct_days_out,
                                int *enjoyment_out)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 now = (sqlite3_int64)time(NULL), last = 0;
    char previous[32] = "inactive";
    int enjoyment = 50, count = 0, days = 0;
    int rc = sqlite3_prepare_v2(addiction_db,
        "SELECT status,enjoyment,COALESCE(last_chosen,0) FROM addiction_targets WHERE target=? COLLATE NOCASE",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            if (v) snprintf(previous, sizeof(previous), "%s", (const char *)v);
            enjoyment = sqlite3_column_int(st, 1);
            last = sqlite3_column_int64(st, 2);
        } else rc = SQLITE_ERROR;
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK) return -1;

    st = NULL;
    rc = sqlite3_prepare_v2(addiction_db,
        "SELECT COUNT(*),COUNT(DISTINCT date(occurred_at,'unixepoch','localtime')) "
        "FROM addiction_events WHERE target=? COLLATE NOCASE AND event_kind='choice' AND occurred_at>=?",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, now - 14 * 86400);
        if (sqlite3_step(st) == SQLITE_ROW) {
            count = sqlite3_column_int(st, 0);
            days = sqlite3_column_int(st, 1);
        } else rc = SQLITE_ERROR;
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK) return -1;

    const char *next = "inactive";
    /* Frequent voluntary choices spread across days matter more than a single
       intense experience. A high enjoyment score alone cannot qualify. */
    if (count >= 8 && days >= 3 && enjoyment >= 75)
        next = "active";
    else if (count >= 4 && days >= 2 && enjoyment >= 60)
        next = "emerging";
    else if (!strcasecmp(previous, "active") && last > 0 && now - last > 7 * 86400)
        next = "dormant";

    if (old_status && old_cap) snprintf(old_status, old_cap, "%s", previous);
    if (new_status && new_cap) snprintf(new_status, new_cap, "%s", next);
    if (changed) *changed = strcasecmp(previous, next) != 0;
    if (choice_count_out) *choice_count_out = count;
    if (distinct_days_out) *distinct_days_out = days;
    if (enjoyment_out) *enjoyment_out = enjoyment;

    st = NULL;
    rc = sqlite3_prepare_v2(addiction_db,
        "UPDATE addiction_targets SET status=?,target_type=?,updated_at=? WHERE target=? COLLATE NOCASE",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, next, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, target_type && *target_type ? target_type : "activity", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now);
        sqlite3_bind_text(st, 4, target, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void log_status_change(const char *target, const char *old_status,
                              const char *new_status, int count, int days,
                              int enjoyment, const char *source)
{
    if (!strcasecmp(old_status, new_status)) return;
    char summary[768], details[1400];
    snprintf(summary, sizeof(summary),
        "R2's modeled relationship with %s changed from %s to %s.",
        target, old_status, new_status);
    snprintf(details, sizeof(details),
        "target=%s; old_status=%s; new_status=%s; voluntary_choices_last_14_days=%d; distinct_choice_days=%d; current_enjoyment=%d/100; source=%s. This is a behavior-derived simulation label, not a clinical diagnosis.",
        target, old_status, new_status, count, days, enjoyment,
        source && *source ? source : "unknown");
    if (r2_log_is_initialized())
        (void)r2_log_event_with_memory(R2_LOG_WORLD, "addiction_status_changed",
            summary, details, "Addiction.c", 1);
}

int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    if (!valid_target(target) || enjoyment_0_100 < 0 || enjoyment_0_100 > 100)
        return -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    char old_status[32] = "inactive", new_status[32] = "inactive";
    int changed = 0, count = 0, days = 0, enjoyment = enjoyment_0_100;
    pthread_mutex_lock(&addiction_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&addiction_lock);
        return -1;
    }
    /* Preserve learned enjoyment across repeated choices. A new target starts
       with the neutral default supplied by the caller; existing targets keep
       their latest learned score until feedback changes it. */
    sqlite3_stmt *st = NULL;
    int lookup = sqlite3_prepare_v2(addiction_db,
        "SELECT enjoyment FROM addiction_targets WHERE target=? COLLATE NOCASE",
        -1, &st, NULL);
    if (lookup == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            enjoyment = sqlite3_column_int(st, 0);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (upsert_target_locked(target, target_type, enjoyment, now) != 0) {
        pthread_mutex_unlock(&addiction_lock);
        return -1;
    }
    int rc = sqlite3_prepare_v2(addiction_db,
        "INSERT INTO addiction_events(target,event_kind,enjoyment,source,details,occurred_at) VALUES(?,'choice',?,?,?,?)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, enjoyment_0_100);
        sqlite3_bind_text(st, 3, source && *source ? source : "unknown", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, details ? details : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, now);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        st = NULL;
        rc = sqlite3_prepare_v2(addiction_db,
            "UPDATE addiction_targets SET last_chosen=?,updated_at=? WHERE target=? COLLATE NOCASE",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            sqlite3_bind_int64(st, 2, now);
            sqlite3_bind_text(st, 3, target, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
    }
    if (rc == SQLITE_DONE)
        rc = update_status_locked(target, target_type, &changed, old_status, sizeof(old_status),
                                  new_status, sizeof(new_status), &count, &days, &enjoyment);
    pthread_mutex_unlock(&addiction_lock);
    if (rc != 0) return -1;
    log_status_change(target, old_status, new_status, count, days, enjoyment, source);
    return 0;
}

int r2_addiction_rate_enjoyment(const char *target, const char *target_type,
                                int enjoyment_0_100, const char *source,
                                const char *details)
{
    if (!valid_target(target) || enjoyment_0_100 < 0 || enjoyment_0_100 > 100)
        return -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    int old_enjoyment = 50, changed = 0, count = 0, days = 0, current = enjoyment_0_100;
    char old_status[32] = "inactive", new_status[32] = "inactive";
    pthread_mutex_lock(&addiction_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&addiction_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(addiction_db,
        "SELECT enjoyment FROM addiction_targets WHERE target=? COLLATE NOCASE",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) old_enjoyment = sqlite3_column_int(st, 0);
        else rc = SQLITE_DONE;
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        pthread_mutex_unlock(&addiction_lock);
        return -1;
    }
    if (upsert_target_locked(target, target_type, enjoyment_0_100, now) != 0) {
        pthread_mutex_unlock(&addiction_lock);
        return -1;
    }
    st = NULL;
    rc = sqlite3_prepare_v2(addiction_db,
        "INSERT INTO addiction_events(target,event_kind,enjoyment,source,details,occurred_at) VALUES(?,'enjoyment',?,?,?,?)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, enjoyment_0_100);
        sqlite3_bind_text(st, 3, source && *source ? source : "unknown", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, details ? details : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, now);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE)
        rc = update_status_locked(target, target_type, &changed, old_status, sizeof(old_status),
                                  new_status, sizeof(new_status), &count, &days, &current);
    pthread_mutex_unlock(&addiction_lock);
    if (rc != 0) return -1;

    if (abs(enjoyment_0_100 - old_enjoyment) >= 10 && r2_log_is_initialized()) {
        char summary[768], event_details[1400];
        snprintf(summary, sizeof(summary), "R2's modeled enjoyment of %s changed from %d/100 to %d/100.",
                 target, old_enjoyment, enjoyment_0_100);
        snprintf(event_details, sizeof(event_details),
                 "target=%s; target_type=%s; previous_enjoyment=%d; current_enjoyment=%d; source=%s; details=%s",
                 target, target_type && *target_type ? target_type : "activity",
                 old_enjoyment, enjoyment_0_100, source && *source ? source : "unknown",
                 details && *details ? details : "none supplied");
        (void)r2_log_event_with_memory(R2_LOG_WORLD, "enjoyment_changed",
            summary, event_details, "Addiction.c", 1);
    }
    log_status_change(target, old_status, new_status, count, days, current, source);
    return 0;
}

char *r2_addiction_report(void)
{
    pthread_mutex_lock(&addiction_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&addiction_lock);
        return strdup("Addiction database unavailable.\n");
    }
    const char *sql =
        "SELECT t.target,t.target_type,t.enjoyment,t.status,"
        "(SELECT COUNT(*) FROM addiction_events e WHERE e.target=t.target COLLATE NOCASE AND e.event_kind='choice' AND e.occurred_at>=?),"
        "(SELECT COUNT(DISTINCT date(e.occurred_at,'unixepoch','localtime')) FROM addiction_events e WHERE e.target=t.target COLLATE NOCASE AND e.event_kind='choice' AND e.occurred_at>=?),"
        "COALESCE(t.last_chosen,0) FROM addiction_targets t ORDER BY t.updated_at DESC";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(addiction_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&addiction_lock);
        return strdup("Could not read addiction records.\n");
    }
    sqlite3_int64 since = (sqlite3_int64)time(NULL) - 14 * 86400;
    sqlite3_bind_int64(st, 1, since);
    sqlite3_bind_int64(st, 2, since);
    size_t cap = 2048, len = 0;
    char *out = malloc(cap);
    if (!out) { sqlite3_finalize(st); pthread_mutex_unlock(&addiction_lock); return NULL; }
    out[0] = '\0';
    int rows = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *target = sqlite3_column_text(st, 0);
        const unsigned char *type = sqlite3_column_text(st, 1);
        const unsigned char *status = sqlite3_column_text(st, 3);
        char line[1024];
        int n = snprintf(line, sizeof(line),
            "%s [%s] enjoyment=%d/100; choices/14d=%d; choice_days=%d; status=%s\n",
            target ? (const char *)target : "unknown",
            type ? (const char *)type : "activity",
            sqlite3_column_int(st, 2), sqlite3_column_int(st, 4),
            sqlite3_column_int(st, 5), status ? (const char *)status : "inactive");
        if (n <= 0) continue;
        if (len + (size_t)n + 1 > cap) {
            size_t next = cap * 2;
            while (next < len + (size_t)n + 1) next *= 2;
            char *grown = realloc(out, next);
            if (!grown) break;
            out = grown; cap = next;
        }
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
        out[len] = '\0';
        rows++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&addiction_lock);
    if (!rows) snprintf(out, cap, "No repeated-interest records yet. A single enjoyable experience does not establish an addiction.\n");
    return out;
}
