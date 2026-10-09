#define _POSIX_C_SOURCE 200809L
#include "r2_diary.h"
#include "Log.h"

#include <assert.h>
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

int main(void)
{
    char *context;
    assert(r2_diary_init() == 0);

    /* This entry is written before the Life Log starts; it must remain
       intact and be linked later rather than being dropped or rewritten. */
    assert(r2_diary_write("Legacy diary entry about learning.") == 0);
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries;") == 1);

    log_ready = 1;
    assert(r2_diary_reconnect_history(10) >= 1);

    assert(r2_diary_write("New reflection connected to history.") == 0);
    assert(scalar_int("SELECT COUNT(*) FROM diary_entries;") == 2);
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

    r2_diary_shutdown();
    puts("Diary/Life Log/memory integration smoke test passed.");
    return 0;
}
