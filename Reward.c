#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Reward.h"
#include "r2_diary.h"
#include "Log.h"
#include "Addiction.h"
#include "r2.h"

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REWARD_DB_PATH R2_DATA_DIR "/r2_rewards.db"
#define REWARD_TARGET_MAX 256
#define REWARD_REASON_MAX 1024
#define REWARD_DECAY_SECONDS (30 * 60)

static sqlite3 *reward_db = NULL;
static pthread_mutex_t reward_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_text(const char *s, size_t max)
{
    return s && *s && strlen(s) <= max;
}

static int reward_column_exists(const char *table, const char *column)
{
    char sql[256];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(reward_db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && strcmp((const char *)name, column) == 0) {
                found = 1;
                break;
            }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
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
        "CREATE TABLE IF NOT EXISTS reward_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, target TEXT NOT NULL,"
        " source TEXT NOT NULL, points INTEGER NOT NULL CHECK(points BETWEEN -7 AND 5 AND points<>0),"
        " reason TEXT NOT NULL, occurred_at INTEGER NOT NULL, modifier INTEGER NOT NULL,"
        " expires_at INTEGER NOT NULL, log_event_id INTEGER, memory_indexed INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS reward_events_target_time ON reward_events(target,occurred_at);",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Schema error: %s\n", sqlite3_errmsg(reward_db));
        sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }

    /* Upgrade the totals row before any statement assumes updated_at exists.
       CREATE TABLE IF NOT EXISTS does not add columns to an older ledger. */
    if (!reward_column_exists("reward_totals", "points") &&
        sqlite3_exec(reward_db,
            "ALTER TABLE reward_totals ADD COLUMN points INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Could not migrate reward totals points: %s\n",
                sqlite3_errmsg(reward_db));
        sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }
    if (!reward_column_exists("reward_totals", "updated_at") &&
        sqlite3_exec(reward_db,
            "ALTER TABLE reward_totals ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0",
            NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Could not migrate reward totals timestamp: %s\n",
                sqlite3_errmsg(reward_db));
        sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }
    if (sqlite3_exec(reward_db,
        "UPDATE reward_totals SET updated_at=CAST(strftime('%s','now') AS INTEGER) "
        "WHERE updated_at IS NULL OR updated_at=0;"
        "INSERT OR IGNORE INTO reward_totals(id,points,updated_at) "
        "VALUES(1,0,CAST(strftime('%s','now') AS INTEGER));",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Reward] Could not initialize reward totals: %s\n",
                sqlite3_errmsg(reward_db));
        sqlite3_close(reward_db);
        reward_db = NULL;
        return -1;
    }

    /* Older ledgers may predate one or both cross-reference columns. */
    const char *required_columns[] = {"log_event_id", "memory_indexed"};
    const char *alter_statements[] = {
        "ALTER TABLE reward_events ADD COLUMN log_event_id INTEGER",
        "ALTER TABLE reward_events ADD COLUMN memory_indexed INTEGER NOT NULL DEFAULT 0"
    };
    for (size_t i = 0; i < sizeof(required_columns) / sizeof(required_columns[0]); ++i) {
        sqlite3_stmt *columns = NULL;
        int found = 0;
        int column_rc = sqlite3_prepare_v2(reward_db,
            "PRAGMA table_info(reward_events)", -1, &columns, NULL);
        if (column_rc != SQLITE_OK) {
            fprintf(stderr, "[R2 Reward] Could not inspect reward schema: %s\n",
                    sqlite3_errmsg(reward_db));
            if (columns) sqlite3_finalize(columns);
            sqlite3_close(reward_db);
            reward_db = NULL;
            return -1;
        }
        while ((column_rc = sqlite3_step(columns)) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(columns, 1);
            if (name && strcmp((const char *)name, required_columns[i]) == 0)
                found = 1;
        }
        sqlite3_finalize(columns);
        if (column_rc != SQLITE_DONE) {
            fprintf(stderr, "[R2 Reward] Could not read reward schema: %s\n",
                    sqlite3_errmsg(reward_db));
            sqlite3_close(reward_db);
            reward_db = NULL;
            return -1;
        }
        if (!found && sqlite3_exec(reward_db, alter_statements[i],
                                    NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[R2 Reward] Could not migrate reward schema: %s\n",
                    sqlite3_errmsg(reward_db));
            sqlite3_close(reward_db);
            reward_db = NULL;
            return -1;
        }
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

typedef struct {
    sqlite3_int64 id;
    sqlite3_int64 log_event_id;
    int memory_indexed;
    char target[REWARD_TARGET_MAX + 1];
    char source[129];
    int points;
    char reason[REWARD_REASON_MAX + 1];
    sqlite3_int64 occurred_at;
    int modifier;
    sqlite3_int64 expires_at;
} PendingReward;

static void format_reward_event(const PendingReward *event,
                                char *summary, size_t summary_size,
                                char *details, size_t details_size)
{
    int duration = (int)(event->expires_at - event->occurred_at);
    if (duration < 0) duration = 0;
    snprintf(summary, summary_size,
        "R2 recorded a %s learning signal (%+d points) for %.240s.",
        event->points > 0 ? "positive" : "corrective",
        event->points, event->target);
    snprintf(details, details_size,
        "reward_event_id=%lld; target=%s; source=%s; points=%+d; reason=%.900s; "
        "temporary_enjoyment_modifier=%+d/100; modifier_expires_after_seconds=%d; "
        "reward_total_is_not_a_measure_of_worth.",
        (long long)event->id, event->target, event->source, event->points,
        event->reason, event->modifier, duration);
}

static int64_t find_existing_log_event(sqlite3_int64 reward_id)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t event_id = 0;
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "reward_event_id=%lld;*", (long long)reward_id);
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 3000);
    if (sqlite3_prepare_v2(db,
        "SELECT id FROM r2_log_events WHERE event_type='enjoyment_changed' "
        "AND details GLOB ? ORDER BY id DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, pattern, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            event_id = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return event_id;
}

static int ensure_reward_memory_pointer(int64_t log_event_id,
                                        const char *summary)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int memory_saved = 0;
    int rc;
    char pointer[2048];

    if (log_event_id <= 0 || !summary) return -1;
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 3000);
    rc = sqlite3_prepare_v2(db,
        "SELECT memory_saved FROM r2_log_events WHERE id=?",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, log_event_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            memory_saved = sqlite3_column_int(st, 0);
            rc = SQLITE_OK;
        } else {
            rc = SQLITE_ERROR;
        }
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    if (rc != SQLITE_OK) return -1;
    if (memory_saved) return 1;

    snprintf(pointer, sizeof(pointer),
        "Life Log event %lld: [world/enjoyment_changed] %.1400s",
        (long long)log_event_id, summary);
    if (r2_save_memory(pointer, "experience") != 0) return 0;

    db = NULL;
    st = NULL;
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 3000);
    rc = sqlite3_prepare_v2(db,
        "UPDATE r2_log_events SET memory_saved=1 WHERE id=?",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, log_event_id);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return rc == SQLITE_DONE ? 1 : -1;
}

static int store_log_link(sqlite3_int64 reward_id, int64_t log_event_id,
                          int memory_indexed)
{
    sqlite3_stmt *st = NULL;
    int rc;
    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&reward_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(reward_db,
        "UPDATE reward_events SET log_event_id=?,memory_indexed=? "
        "WHERE id=? AND (log_event_id IS NULL OR log_event_id=0 OR memory_indexed=0)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, log_event_id);
        sqlite3_bind_int(st, 2, memory_indexed ? 1 : 0);
        sqlite3_bind_int64(st, 3, reward_id);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Persist the reward event, total, and optional idempotency key in one
 * SQLite transaction. A durable once-key without its reward row is not a
 * completed reward: older versions could leave that state after a crash. */
static int reward_apply_internal(const char *target, const char *source, int points,
                                 const char *reason, int voluntary_choice,
                                 int apply_once)
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
    sqlite3_int64 reward_id = 0;
    sqlite3_stmt *st = NULL;
    int duplicate = 0;
    PendingReward prior_event = {0};

    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&reward_lock);
        return -1;
    }

    if (apply_once && sqlite3_exec(reward_db,
            "CREATE TABLE IF NOT EXISTS reward_once_keys ("
            "target TEXT NOT NULL, source TEXT NOT NULL, created_at INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(target,source));",
            NULL, NULL, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&reward_lock);
        return -1;
    }
    /* Earlier idempotency-key tables had only target/source. Add the timestamp
       in place before any INSERT assumes it exists; retain every existing key. */
    if (apply_once &&
        ((!reward_column_exists("reward_once_keys", "created_at") &&
          sqlite3_exec(reward_db,
              "ALTER TABLE reward_once_keys ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0",
              NULL, NULL, NULL) != SQLITE_OK) ||
         sqlite3_exec(reward_db,
              "UPDATE reward_once_keys SET created_at=CAST(strftime('%s','now') AS INTEGER) "
              "WHERE created_at IS NULL OR created_at=0",
              NULL, NULL, NULL) != SQLITE_OK)) {
        fprintf(stderr, "[R2 Reward] Could not migrate reward idempotency keys: %s\n",
                sqlite3_errmsg(reward_db));
        pthread_mutex_unlock(&reward_lock);
        return -1;
    }

    rc = sqlite3_exec(reward_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc == SQLITE_OK && apply_once) {
        /* First check the durable event as well as the key. This repairs old
           key-only rows left by the former two-transaction implementation. */
        rc = sqlite3_prepare_v2(reward_db,
            "SELECT id,target,source,points,reason,occurred_at,modifier,expires_at "
            "FROM reward_events WHERE target=? AND source=? LIMIT 1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, source, -1, SQLITE_TRANSIENT);
            int step_rc = sqlite3_step(st);
            if (step_rc == SQLITE_ROW) {
                const unsigned char *saved_target = sqlite3_column_text(st, 1);
                const unsigned char *saved_source = sqlite3_column_text(st, 2);
                const unsigned char *saved_reason = sqlite3_column_text(st, 4);
                prior_event.id = sqlite3_column_int64(st, 0);
                snprintf(prior_event.target, sizeof(prior_event.target), "%s",
                    saved_target ? (const char *)saved_target : target);
                snprintf(prior_event.source, sizeof(prior_event.source), "%s",
                    saved_source ? (const char *)saved_source : source);
                prior_event.points = sqlite3_column_int(st, 3);
                snprintf(prior_event.reason, sizeof(prior_event.reason), "%s",
                    saved_reason ? (const char *)saved_reason : "");
                prior_event.occurred_at = sqlite3_column_int64(st, 5);
                prior_event.modifier = sqlite3_column_int(st, 6);
                prior_event.expires_at = sqlite3_column_int64(st, 7);
                duplicate = 1;
                rc = SQLITE_OK;
            } else {
                rc = step_rc == SQLITE_DONE ? SQLITE_OK : step_rc;
            }
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }

    if (rc == SQLITE_OK && apply_once && !duplicate) {
        rc = sqlite3_prepare_v2(reward_db,
            "INSERT OR IGNORE INTO reward_once_keys(target,source,created_at) VALUES(?,?,?);",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, source, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 3, now);
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }

    if (rc == SQLITE_OK && !duplicate) {
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
            if (rc == SQLITE_DONE) {
                reward_id = sqlite3_last_insert_rowid(reward_db);
                rc = SQLITE_OK;
            }
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }

    if (rc == SQLITE_OK && !duplicate) {
        rc = sqlite3_prepare_v2(reward_db,
            "UPDATE reward_totals SET points=points+?,updated_at=? WHERE id=1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(st, 1, points);
            sqlite3_bind_int64(st, 2, now);
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }

    if (rc == SQLITE_OK) {
        int commit_rc = sqlite3_exec(reward_db, "COMMIT;", NULL, NULL, NULL);
        if (commit_rc != SQLITE_OK) {
            (void)sqlite3_exec(reward_db, "ROLLBACK;", NULL, NULL, NULL);
            rc = commit_rc;
        }
    } else {
        (void)sqlite3_exec(reward_db, "ROLLBACK;", NULL, NULL, NULL);
    }
    pthread_mutex_unlock(&reward_lock);

    if (rc != SQLITE_OK) return -1;
    if (duplicate) {
        if (!apply_once) return -1;
        /* Repair the addiction-choice side effect if the prior process died
           after committing the reward but before recording the choice. Its
           stable reward_event_id makes the replay idempotent. */
        if (voluntary_choice && prior_event.id > 0) {
            char prior_summary[512], prior_details[1600];
            format_reward_event(&prior_event, prior_summary, sizeof(prior_summary),
                                prior_details, sizeof(prior_details));
            int choice_rc = r2_addiction_record_choice(prior_event.target, "activity",
                prior_event.points >= 0 ? (50 + prior_event.points * 5)
                                        : (50 + prior_event.points * 4),
                "reinforcement", prior_details);
            if (choice_rc != 0) return -1;
        }
        return 1;
    }
    if (reward_id <= 0) return -1;

    /* External side effects happen only after the durable reward transaction.
       Startup reconciliation can reconstruct Life Log links after a crash. */
    PendingReward event = {0};
    event.id = reward_id;
    snprintf(event.target, sizeof(event.target), "%s", target);
    snprintf(event.source, sizeof(event.source), "%s", source);
    event.points = points;
    snprintf(event.reason, sizeof(event.reason), "%s", reason);
    event.occurred_at = now;
    event.modifier = modifier;
    event.expires_at = expires;
    char summary[512], details[1600];
    format_reward_event(&event, summary, sizeof(summary), details, sizeof(details));

    if (r2_log_is_initialized()) {
        int64_t log_event_id = r2_log_event_with_memory(R2_LOG_WORLD,
            "enjoyment_changed", summary, details, "Reward.c", 0);
        if (log_event_id > 0) {
            int memory_indexed = ensure_reward_memory_pointer(log_event_id, summary);
            (void)store_log_link(reward_id, log_event_id, memory_indexed > 0);
        }
    }

    if (voluntary_choice)
        (void)r2_addiction_record_choice(target, "activity",
            points >= 0 ? (50 + points * 5) : (50 + points * 4),
            "reinforcement", details);
    return 0;
}

int r2_reward_apply(const char *target, const char *source, int points,
                    const char *reason, int voluntary_choice)
{
    return reward_apply_internal(target, source, points, reason,
                                 voluntary_choice, 0);
}

int r2_reward_apply_once(const char *target, const char *source, int points,
                         const char *reason, int voluntary_choice)
{
    return reward_apply_internal(target, source, points, reason,
                                 voluntary_choice, 1);
}

int r2_reward_reconnect_history(int limit)
{
    PendingReward *pending = NULL;
    sqlite3_stmt *st = NULL;
    int rc, count = 0, linked = 0;
    if (!r2_log_is_initialized()) return -1;
    if (limit <= 0) limit = 100;
    if (limit > 500) limit = 500;

    pending = calloc((size_t)limit, sizeof(*pending));
    if (!pending) return -1;
    pthread_mutex_lock(&reward_lock);
    if (ensure_db_locked() != 0) {
        pthread_mutex_unlock(&reward_lock);
        free(pending);
        return -1;
    }
    rc = sqlite3_prepare_v2(reward_db,
        "SELECT id,COALESCE(log_event_id,0),memory_indexed,target,source,points,reason,occurred_at,modifier,expires_at "
        "FROM reward_events WHERE log_event_id IS NULL OR log_event_id=0 OR memory_indexed=0 "
        "ORDER BY id LIMIT ?", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW && count < limit) {
            PendingReward *item = &pending[count++];
            const unsigned char *target_text = sqlite3_column_text(st, 3);
            const unsigned char *source_text = sqlite3_column_text(st, 4);
            const unsigned char *reason_text = sqlite3_column_text(st, 6);
            item->id = sqlite3_column_int64(st, 0);
            item->log_event_id = sqlite3_column_int64(st, 1);
            item->memory_indexed = sqlite3_column_int(st, 2);
            snprintf(item->target, sizeof(item->target), "%s",
                target_text ? (const char *)target_text : "unknown");
            snprintf(item->source, sizeof(item->source), "%s",
                source_text ? (const char *)source_text : "unknown");
            item->points = sqlite3_column_int(st, 5);
            snprintf(item->reason, sizeof(item->reason), "%s",
                reason_text ? (const char *)reason_text : "");
            item->occurred_at = sqlite3_column_int64(st, 7);
            item->modifier = sqlite3_column_int(st, 8);
            item->expires_at = sqlite3_column_int64(st, 9);
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(pending);
        return -1;
    }

    for (int i = 0; i < count; ++i) {
        PendingReward *item = &pending[i];
        int64_t log_event_id = item->log_event_id > 0
            ? item->log_event_id : find_existing_log_event(item->id);
        char summary[512], details[1600];
        format_reward_event(item, summary, sizeof(summary), details, sizeof(details));
        if (log_event_id <= 0)
            log_event_id = r2_log_event_with_memory(R2_LOG_WORLD,
                "enjoyment_changed", summary, details, "Reward.c", 0);
        if (log_event_id > 0) {
            int memory_indexed = ensure_reward_memory_pointer(log_event_id, summary);
            if (store_log_link(item->id, log_event_id, memory_indexed > 0) == 0 &&
                memory_indexed > 0)
                linked++;
        }
    }
    free(pending);
    return linked;
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
            double remaining = 0.0;
            if (expires > now && expires > occurred) {
                remaining = occurred > now ? 1.0 :
                    (double)(expires - now) / (double)(expires - occurred);
                if (remaining < 0.0) remaining = 0.0;
                if (remaining > 1.0) remaining = 1.0;
            }
            int modifier = (int)(points * 2.0 * remaining);
            n = snprintf(out + used, cap - used,
                "Recent: target=%s; points=%+d; temporary_modifier=%+d; reason=%.160s\n",
                target ? target : "unknown", points, modifier, reason ? reason : "");
            if (n > 0 && (size_t)n < cap - used) used += (size_t)n;
            else break;
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reward_lock);
    return out;
}

int r2_reward_review_diary(int64_t diary_entry_id, const char *entry)
{
    if (diary_entry_id <= 0 || !entry) return -1;
    /*
     * Diary feedback is intentionally generic. Do not hard-code a person's
     * name or a particular identity correction into R2's learning loop.
     * Successfully persisted diary entries receive the normal +1 signal.
     */
    char reason[160];
    snprintf(reason, sizeof(reason), "Diary entry #%lld was successfully persisted.",
             (long long)diary_entry_id);
    return r2_reward_apply("diary_writing", "diary_review", 1, reason, 0);
}
