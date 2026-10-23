#define _POSIX_C_SOURCE 200809L
#include "r2_diary.h"
#include "Log.h"
#include "Reward.h"

#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static int log_ready = 0;

int r2_log_is_initialized(void)
{
    return log_ready;
}

int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details;
    return 0;
}

static int reward_total(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int value = -99999;
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK) return value;
    if (sqlite3_prepare_v2(db, "SELECT points FROM reward_totals WHERE id=1",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int reward_link_count(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int value = -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK) return value;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM reward_events WHERE log_event_id IS NOT NULL",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int reward_memory_count(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int value = -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK) return value;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM reward_events WHERE memory_indexed=1",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int open_test_db(sqlite3 **db)
{
    if (sqlite3_open(R2_DIARY_DATABASE, db) != SQLITE_OK)
        return -1;
    sqlite3_busy_timeout(*db, 3000);
    return sqlite3_exec(*db,
        "CREATE TABLE IF NOT EXISTS r2_log_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "local_time TEXT NOT NULL,"
        "category TEXT NOT NULL,"
        "event_type TEXT NOT NULL,"
        "summary TEXT NOT NULL,"
        "details TEXT,"
        "source TEXT,"
        "memory_saved INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS memories ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "memory TEXT NOT NULL,"
        "category TEXT NOT NULL,"
        "UNIQUE(memory,category));",
        NULL, NULL, NULL);
}

int r2_save_memory(const char *memory, const char *category)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    if (!memory || open_test_db(&db) != 0) return -1;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO memories(memory,category) VALUES(?,?);",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, memory, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, category ? category : "experience",
                          -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

int64_t r2_log_event_with_memory(R2LogCategory category,
    const char *event_type, const char *summary, const char *details,
    const char *source, int save_as_memory)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int rc;
    int64_t id = -1;
    const char *category_name = category == R2_LOG_THINKING ? "thinking" : "other";
    if (open_test_db(&db) != 0) return -1;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO r2_log_events(local_time,category,event_type,summary,details,source)"
        " VALUES(datetime('now','localtime'),?,?,?,?,?);",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, category_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, event_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, summary, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, details ? details : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, source ? source : "", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) id = sqlite3_last_insert_rowid(db);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    if (rc != SQLITE_DONE || id <= 0) return -1;

    if (save_as_memory) {
        char pointer[1024];
        snprintf(pointer, sizeof(pointer),
            "Life Log event %lld: [%s/%s] %s",
            (long long)id, category_name, event_type, summary);
        if (r2_save_memory(pointer, "experience") == 0) {
            if (open_test_db(&db) == 0) {
                if (sqlite3_prepare_v2(db,
                    "UPDATE r2_log_events SET memory_saved=1 WHERE id=?;",
                    -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(st, 1, id);
                    sqlite3_step(st);
                }
                if (st) sqlite3_finalize(st);
                sqlite3_close(db);
            }
        }
    }
    return id;
}

char *r2_retrieve_memories(const char *query)
{
    (void)query;
    return strdup("Smoke-test retrieved memory: prior learning is available.");
}

static int scalar_int(const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int value = -1;
    if (open_test_db(&db) != 0) return -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int reward_scalar_int(const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int value = -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int create_legacy_reward_db(void)
{
    sqlite3 *db = NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *legacy_schema =
        "CREATE TABLE reward_totals (id INTEGER PRIMARY KEY CHECK(id=1),"
        " points INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE reward_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, target TEXT NOT NULL,"
        " source TEXT NOT NULL, points INTEGER NOT NULL CHECK(points BETWEEN -7 AND 5 AND points<>0),"
        " reason TEXT NOT NULL, occurred_at INTEGER NOT NULL, modifier INTEGER NOT NULL,"
        " expires_at INTEGER NOT NULL);"
        "CREATE INDEX reward_events_target_time ON reward_events(target,occurred_at);"
        "CREATE TABLE reward_once_keys(target TEXT NOT NULL,source TEXT NOT NULL,"
        "PRIMARY KEY(target,source));"
        "INSERT INTO reward_once_keys(target,source) VALUES('legacy-key-target','legacy-key-source');";
    int rc = sqlite3_exec(db, legacy_schema, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static void seed_legacy_diary_entries(void)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE diary_entries ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "content TEXT NOT NULL,"
        "timestamp TEXT NOT NULL);"
        "INSERT INTO diary_entries(content,timestamp) "
        "VALUES('legacy schema migration fixture','2001-02-03 04:05:06');",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
}

static void verify_legacy_diary_migration(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db,
        "SELECT entry,created_at FROM diary_entries WHERE entry='legacy schema migration fixture'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const unsigned char *entry = sqlite3_column_text(st, 0);
    const unsigned char *created = sqlite3_column_text(st, 1);
    assert(entry && strcmp((const char *)entry, "legacy schema migration fixture") == 0);
    assert(created && strcmp((const char *)created, "2001-02-03 04:05:06") == 0);
    sqlite3_finalize(st);
    assert(sqlite3_exec(db,
        "DELETE FROM diary_entries WHERE entry='legacy schema migration fixture'",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
}

int main(void)
{
    char *context;
    assert(mkdir(R2_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    seed_legacy_diary_entries();
    assert(r2_diary_init() == 0);
    verify_legacy_diary_migration();
    /* Simulate a ledger created before reward-to-Life-Log columns existed. */
    assert(create_legacy_reward_db() == 0);

    /* This entry is written before the Life Log starts; it must remain
       intact and be linked later rather than being dropped or rewritten. */
    assert(r2_diary_write("Legacy diary entry about learning.") == 0);
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries;") == 1);
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries "
                      "WHERE timestamp=created_at AND timestamp<>'';
    /* The legacy content column stays NOT NULL after migration; writes must fill it. */
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries "
                      "WHERE content=entry AND content<>'';") == 1);") == 1);

    log_ready = 1;
    assert(r2_diary_reconnect_history(10) >= 1);
    assert(r2_reward_reconnect_history(10) >= 1);

    assert(r2_diary_write("New reflection connected to history.") == 0);
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries;") == 2);

    /* Diary review applies generic +1 feedback without name-specific rules. */
    assert(r2_reward_review_diary(100, "I learned something from this experience.") == 0);
    assert(r2_reward_review_diary(101, "I will reflect on what happened.") == 0);
    assert(r2_reward_review_diary(102, "I believe learning takes time.") == 0);
    assert(reward_total() == 5);
    assert(reward_link_count() == 5);
    assert(reward_memory_count() == 5);
    assert(r2_reward_reconnect_history(10) == 0);
    assert(scalar_int("SELECT COUNT(*) FROM r2_log_events "
                      "WHERE event_type='enjoyment_changed';") == 5);
    assert(r2_reward_current_modifier("diary_writing") >= 0);
    assert(r2_reward_current_modifier("diary_writing") <= 10);
    assert(scalar_int("SELECT COUNT(*) FROM r2_diary_entry_links "
                      "WHERE log_event_id IS NOT NULL;") == 2);
    assert(scalar_int("SELECT COUNT(*) FROM r2_diary_entry_links "
                      "WHERE memory_indexed=1;") == 2);
    assert(scalar_int("SELECT COUNT(*) FROM r2_log_events "
                      "WHERE event_type='diary_entry_linked';") == 2);
    /* The event contains only a pointer; private diary prose is not copied
       into the factual Life Log or Observer-facing summaries. */
    assert(scalar_int("SELECT COUNT(*) FROM r2_log_events "
                      "WHERE details LIKE '%Legacy diary entry about learning%' "
                      "OR summary LIKE '%Legacy diary entry about learning%';") == 0);
    context = r2_diary_build_reflection_context(10);
    assert(context != NULL);
    assert(strstr(context, "Legacy diary entry about learning.") != NULL);
    assert(strstr(context, "New reflection connected to history.") != NULL);
    assert(strstr(context, "RECENT LIFE LOG EVENTS") != NULL);
    assert(strstr(context, "RELEVANT PERSISTENT MEMORIES") != NULL);
    assert(strstr(context, "Smoke-test retrieved memory") != NULL);
    free(context);

    /* Reconciliation is idempotent: a second pass must not duplicate events. */
    assert(r2_diary_reconnect_history(10) == 0);
    assert(scalar_int("SELECT COUNT(*) FROM r2_log_events "
                      "WHERE event_type='diary_entry_linked';") == 2);

    /* The old key-only table is migrated lazily when the idempotent API is
       first used. Replaying the same reward must not increment totals twice. */
    int before_legacy_probe = reward_total();
    assert(r2_reward_apply_once("legacy-key-target", "legacy-key-source", 1,
                                "Legacy idempotency migration probe", 0) == 0);
    assert(reward_total() == before_legacy_probe + 1);
    assert(r2_reward_apply_once("legacy-key-target", "legacy-key-source", 1,
                                "Legacy idempotency migration probe", 0) == 1);
    assert(reward_total() == before_legacy_probe + 1);
    assert(reward_scalar_int("SELECT COUNT(*) FROM pragma_table_info('reward_once_keys') "
                             "WHERE name='created_at';") == 1);
    assert(reward_scalar_int("SELECT COUNT(*) FROM reward_once_keys "
                             "WHERE target='legacy-key-target' AND source='legacy-key-source' "
                             "AND created_at>0;") == 1);

    r2_diary_shutdown();
    puts("Diary/Life Log/memory integration smoke test passed.");
    return 0;
}
