#define _POSIX_C_SOURCE 200809L
#include "SensoryJournal.h"
#include "Log.h"
#include "r2_diary.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sqlite3.h>

static int exec_sql(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite test setup failed: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

static int insert_event(sqlite3 *db, const char *summary, const char *details)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO r2_log_events(utc_time,local_time,category,event_type,summary,details,source) "
        "VALUES(datetime('now'),datetime('now'),'sensory','observation',?,?, 'smoke-test');";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, details, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* SensoryJournal.c calls the real Life Log for explicit notes. This isolated
   query test deliberately does not exercise note-writing. */
int64_t r2_log_thinking(const char *thought_kind, const char *summary,
                        const char *details)
{
    (void)thought_kind;
    (void)summary;
    (void)details;
    return -1;
}

int main(void)
{
    (void)mkdir(R2_ROOT, 0700);
    (void)mkdir(R2_HOME, 0700);
    remove(R2_DIARY_DATABASE);
    char sidecar[sizeof(R2_DIARY_DATABASE) + 8];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", R2_DIARY_DATABASE);
    remove(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", R2_DIARY_DATABASE);
    remove(sidecar);

    sqlite3 *db = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(exec_sql(db,
        "CREATE TABLE r2_log_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, utc_time TEXT, local_time TEXT,"
        "category TEXT NOT NULL, event_type TEXT NOT NULL, summary TEXT NOT NULL,"
        "details TEXT, source TEXT);"
        "CREATE TABLE diary_entries (id INTEGER PRIMARY KEY, entry TEXT);") == 0);
    assert(insert_event(db, "Saw 100% battery", "capacity was 100%") == 0);
    assert(insert_event(db, "Saw 100 percent battery", "capacity was described in words") == 0);
    assert(insert_event(db, "Saw sensor_port value", "literal underscore in sensor_port") == 0);
    assert(insert_event(db, "Saw sensorXport value", "one-character variant only") == 0);
    assert(insert_event(db, "Saw bang!mark", "literal escape character") == 0);
    sqlite3_close(db);

    assert(r2_sj_init() == 0);
    assert(r2_sj_count() == 5);

    char *found = r2_sj_search("100%", 20);
    assert(found);
    assert(strstr(found, "Saw 100% battery"));
    assert(!strstr(found, "Saw 100 percent battery"));
    free(found);

    found = r2_sj_search("sensor_port", 20);
    assert(found);
    assert(strstr(found, "Saw sensor_port value"));
    assert(!strstr(found, "Saw sensorXport value"));
    free(found);

    found = r2_sj_search("bang!mark", 20);
    assert(found);
    assert(strstr(found, "Saw bang!mark"));
    free(found);

    found = r2_sj_search("not-present", 20);
    assert(found && strstr(found, "No Sensory Journal entries matched."));
    free(found);

    r2_sj_shutdown();
    remove(R2_DIARY_DATABASE);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", R2_DIARY_DATABASE);
    remove(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", R2_DIARY_DATABASE);
    remove(sidecar);
    puts("Sensory Journal literal-search smoke passed");
    return 0;
}
