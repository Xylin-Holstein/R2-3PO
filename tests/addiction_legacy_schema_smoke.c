#define _POSIX_C_SOURCE 200809L
#include "Addiction.h"
#include "Log.h"
#include "r2_diary.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sqlite3.h>

int r2_log_is_initialized(void) { return 0; }
int64_t r2_log_event_with_memory(R2LogCategory category, const char *event_type,
    const char *summary, const char *details, const char *source, int save_as_memory)
{
    (void)category; (void)event_type; (void)summary; (void)details;
    (void)source; (void)save_as_memory;
    return -1;
}

static int scalar_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    return value;
}

int main(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_addictions.db", R2_HOME);
    assert(mkdir(R2_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    const char *legacy =
        "CREATE TABLE addiction_targets ("
        "target TEXT PRIMARY KEY COLLATE NOCASE,"
        "enjoyment INTEGER NOT NULL DEFAULT 50,"
        "status TEXT NOT NULL DEFAULT 'inactive',"
        "first_seen INTEGER NOT NULL,last_chosen INTEGER);"
        "INSERT INTO addiction_targets(target,enjoyment,status,first_seen,last_chosen)"
        " VALUES('old target',73,'inactive',100,NULL);"
        "CREATE TABLE addiction_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,target TEXT NOT NULL,"
        "event_kind TEXT NOT NULL,enjoyment INTEGER NOT NULL,occurred_at INTEGER NOT NULL);"
        "INSERT INTO addiction_events(target,event_kind,enjoyment,occurred_at)"
        " VALUES('old target','choice',73,100);";
    assert(sqlite3_exec(db, legacy, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);

    assert(r2_addiction_init() == 0);
    assert(r2_addiction_record_choice("old target", "activity", 73,
                                      "legacy-smoke", "new write after migration") == 0);
    r2_addiction_shutdown();

    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_int(db, "SELECT COUNT(*) FROM addiction_events") == 2);
    assert(scalar_int(db, "SELECT COUNT(*) FROM addiction_events WHERE source='legacy-smoke'") == 1);
    assert(scalar_int(db, "SELECT COUNT(*) FROM addiction_targets WHERE target='old target' AND enjoyment=73") == 1);

    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA table_info(addiction_events)",
                              -1, &st, NULL) == SQLITE_OK);
    int has_source = 0, has_details = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, "source") == 0) has_source = 1;
        if (name && strcmp((const char *)name, "details") == 0) has_details = 1;
    }
    sqlite3_finalize(st);
    assert(has_source && has_details);
    sqlite3_close(db);
    puts("Addiction legacy-schema migration smoke passed.");
    return 0;
}
