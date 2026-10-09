#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Reward.h"
#include "r2_diary.h"
#include "Log.h"
#include "Addiction.h"

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REWARD_DB_PATH R2_HOME "/r2_rewards.db"
#define REWARD_TARGET_MAX 256
#define REWARD_REASON_MAX 1024
#define REWARD_DECAY_SECONDS (30 * 60)

static sqlite3 *reward_db = NULL;
static pthread_mutex_t reward_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_text(const char *s, size_t max)
{
    return s && *s && strlen(s) <= max;
}

static int ensure_db_locked(void)
{
    if (reward_db) return 0;
    if (sqlite3_open_v2(REWARD_DB_PATH, &reward_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Cannot open %s: %s\n", REWARD_DB_PATH,
                reward_db ? sqlite3_errmsg(reward_db) : "unknown error");
        if (reward_db) sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(reward_db, 5000);
    if (sqlite3_exec(reward_db,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS reward_totals ("
        " id INTEGER PRIMARY KEY CHECK(id=1), points INTEGER NOT NULL DEFAULT 0,"
        " updated_at INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO reward_totals(id,points,updated_at) VALUES(1,0,unixepoch());"
        "CREATE TABLE IF NOT EXISTS reward_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, target TEXT NOT NULL,"
        " source TEXT NOT NULL, points INTEGER NOT NULL CHECK(points BETWEEN -7 AND 5 AND points<>0),"
        " reason TEXT NOT NULL, occurred_at INTEGER NOT NULL, modifier INTEGER NOT NULL,"
        " expires_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS reward_events_target_time ON reward_events(target,occurred_at);",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Schema error: %s\n", sqlite3_errmsg(reward_db));
        sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }
    return 0;
}

int r2_reward_init(void)
{
    pthread_mutex_lock(&reward_lock);
    int rc = ensure_db_locked();
    pthread_mutex_unlock(&reward_lock);
    return rc;
}

void r2_reward_shutdown(void)
{
    pthread_mutex_lock(&reward_lock);
    if (reward_db) {
        sqlite3_wal_checkpoint_v2(reward_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(reward_db);
        reward_db = NULL;
    }
    pthread_mutex_unlock(&reward_lock);
}

int r2_reward_apply(const char *target, const char *source, int points,
                    const char *reason, int voluntary_choice)
{
    if (!valid_text(target, REWARD_TARGET_MAX) ||
        !valid_text(source, 128) || !valid_text(reason, REWARD_REASON_MAX) ||
        points == 0 || points < -7 || points > 5)
        return -1;

    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    int modifier = points * 2;
    if (modifier > 10) modifier = 10;
    if (modifier < -14) modifier = -14;
    sqlite3_int64 expires = now + REWARD_DECAY_SECONDS;
    int rc;
    sqlite3_stmt *st = NULL;

    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&reward_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(reward_db,
        "INSERT INTO reward_events(target,source,points,reason,occurred_at,modifier,expires_at)"
        " VALUES(?,?,?,?,?,?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, points);
        sqlite3_bind_text(st, 4, reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, now);
        sqlite3_bind_int(st, 6, modifier);
        sqlite3_bind_int64(st, 7, expires);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(reward_db,
            "UPDATE reward_totals SET points=points+?,updated_at=? WHERE id=1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(st, 1, points);
            sqlite3_bind_int64(st, 2, now);
            rc = sqlite3_step(st);
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    if (rc != SQLITE_DONE) return -1;

    char summary[512], details[1600];
    snprintf(summary, sizeof(summary), "R2 recorded a %s learning signal (%+d points) for %s.",
             points > 0 ? "positive" : "corrective", points, target);
    snprintf(details, sizeof(details),
        "target=%s; source=%s; points=%+d; reason=%.900s; temporary_enjoyment_modifier=%+d/100; "
        "modifier_expires_after_seconds=%d; reward_total_is_not_a_measure_of_worth.",
        target, source, points, reason, modifier, REWARD_DECAY_SECONDS);

    /* Public summary is a generic state change; private diary text is never
       passed here. The detailed reason should therefore be a short, nonprivate
       outcome description supplied by the calling subsystem. */
    if (r2_log_is_initialized())
        (void)r2_log_event_with_memory(R2_LOG_WORLD, "enjoyment_changed",
            summary, details, "Reward.c", 1);

    if (voluntary_choice)
        (void)r2_addiction_record_choice(target, "activity",
            points >= 0 ? (50 + points * 5) : (50 + points * 4),
            "reinforcement", details);
    return 0;
}

int r2_reward_current_modifier(const char *target)
{
    sqlite3_stmt *st = NULL;
    int result = 0;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    if (!valid_text(target, REWARD_TARGET_MAX)) return 0;
    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() == 0 &&
        sqlite3_prepare_v2(reward_db,
            "SELECT modifier,occurred_at,expires_at FROM reward_events "
            "WHERE target=? COLLATE NOCASE AND expires_at>? ORDER BY id DESC LIMIT 1",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, now);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int modifier = sqlite3_column_int(st, 0);
            sqlite3_int64 start = sqlite3_column_int64(st, 1);
            sqlite3_int64 end = sqlite3_column_int64(st, 2);
            if (end > start) {
                double remaining = (double)(end - now) / (double)(end - start);
                if (remaining < 0.0) remaining = 0.0;
                if (remaining > 1.0) remaining = 1.0;
                result = (int)(modifier * remaining);
            }
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    return result;
}

char *r2_reward_context(void)
{
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    const size_t cap = 4096;
    size_t used = 0;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&reward_lock);
        return NULL;
    }
    out = malloc(cap);
    if (!out) {
        pthread_mutex_unlock(&reward_lock);
        return NULL;
    }
    out[0] = '\0';
    int total = 0;
    if (sqlite3_prepare_v2(reward_db,
        "SELECT points FROM reward_totals WHERE id=1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        total = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    st = NULL;
    int n = snprintf(out, cap,
        "LEARNING/REINFORCEMENT CONTEXT (simulated feedback, not objective worth):\n"
        "Cumulative points: %d. Positive feedback is +1..+5; corrective feedback is at most -7.\n"
        "Temporary enjoyment modifiers decay over 30 minutes and must not be treated as permanent preferences.\n",
        total);
    if (n > 0) used = (size_t)n;
    if (sqlite3_prepare_v2(reward_db,
        "SELECT target,points,reason,occurred_at,expires_at FROM reward_events "
        "ORDER BY id DESC LIMIT 8", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW && used + 256 < cap) {
            const char *target = (const char *)sqlite3_column_text(st, 0);
            int points = sqlite3_column_int(st, 1);
            const char *reason = (const char *)sqlite3_column_text(st, 2);
            sqlite3_int64 occurred = sqlite3_column_int64(st, 3);
            sqlite3_int64 expires = sqlite3_column_int64(st, 4);
            int modifier = expires > now ? (int)(points * 2.0 *
                (double)(expires - now) / (double)REWARD_DECAY_SECONDS) : 0;
            n = snprintf(out + used, cap - used,
                "Recent: target=%s; points=%+d; temporary_modifier=%+d; reason=%.160s\n",
                target ? target : "unknown", points, modifier, reason ? reason : "");
            if (n > 0 && (size_t)n < cap - used) used += (size_t)n;
            else break;
            if (occurred > now) { /* Defensive against a future clock skew. */ }
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    return out;
}

static int has_case_insensitive(const char *text, const char *needle)
{
    return text && needle && strcasestr(text, needle) != NULL;
}

int r2_reward_review_diary(int64_t diary_entry_id, const char *entry)
{
    if (diary_entry_id <= 0 || !entry) return -1;
    /* Narrow correction rule from the active continuity issue: mention of
       "Eli" is corrective only when the entry explicitly says that Eli is
       not real/nonexistent and redirects identity to the user/creator. */
    if (has_case_insensitive(entry, "eli")) {
        int negates = has_case_insensitive(entry, "eli isn't real") ||
            has_case_insensitive(entry, "eli is not real") ||
            has_case_insensitive(entry, "eli does not exist") ||
            has_case_insensitive(entry, "eli doesn't exist") ||
            has_case_insensitive(entry, "eli was never real");
        int redirects = has_case_insensitive(entry, "the user") ||
            has_case_insensitive(entry, "your creator") ||
            has_case_insensitive(entry, "the creator") ||
            has_case_insensitive(entry, "you are") ||
            has_case_insensitive(entry, "you,") ||
            has_case_insensitive(entry, "you.");
        if (negates && redirects)
            return r2_reward_apply("diary_identity_correction", "diary_review", 5,
                "Diary explicitly corrects the recurring identity attribution and redirects it to the intended user/creator.", 0);
        char reason[256];
        snprintf(reason, sizeof(reason),
            "Diary entry #%lld repeats the configured Eli identity-attribution mistake.",
            (long long)diary_entry_id);
        return r2_reward_apply("diary_identity_correction", "diary_review", -7,
            reason, 0);
    }
    return r2_reward_apply("diary_writing", "diary_review", 1,
        "Diary entry was successfully persisted.", 0);
}
