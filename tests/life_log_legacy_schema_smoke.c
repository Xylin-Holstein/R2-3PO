#define _GNU_SOURCE
#include "Log.h"
#include "SensoryJournal.h"
#include "Addiction.h"
#include "r2.h"

#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef R2_DIARY_DATABASE
#error R2_DIARY_DATABASE must point to an isolated test database
#endif

void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id; (void)category; (void)event_type; (void)source;
}

int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category;
    return 0;
}

int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details;
    return 0;
}

static void remove_if_present(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        perror(path);
        exit(1);
    }
}

static int scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

int main(void)
{
    char sidecar[sizeof(R2_DIARY_DATABASE) + 8];
    remove_if_present(R2_DIARY_DATABASE);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", R2_DIARY_DATABASE);
    remove_if_present(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", R2_DIARY_DATABASE);
    remove_if_present(sidecar);

    sqlite3 *legacy = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &legacy) == SQLITE_OK);
    assert(sqlite3_exec(legacy,
        "CREATE TABLE r2_log_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "summary TEXT NOT NULL,details TEXT);"
        "INSERT INTO r2_log_events(id,summary,details) VALUES("
        "7,'legacy preserved identity record',"
        "'Existing Life Log content must remain readable after additive migration.');",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(legacy);

    assert(r2_log_init() == 0);
    char *old_record = r2_log_search("legacy preserved identity", 10);
    assert(old_record != NULL);
    assert(strstr(old_record, "legacy preserved identity record") != NULL);
    free(old_record);

    int64_t new_id = r2_log_event(
        R2_LOG_MEMORY, "post_migration_write",
        "new event after legacy migration",
        "A writable post-migration event.", "legacy-schema-smoke");
    assert(new_id > 0);

    char *new_record = r2_log_search("new event after legacy migration", 10);
    assert(new_record != NULL);
    assert(strstr(new_record, "new event after legacy migration") != NULL);
    free(new_record);

    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_events WHERE id=7 "
        "AND summary='legacy preserved identity record'") == 1);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_events WHERE event_type='post_migration_write'") == 1);
    sqlite3_stmt *integrity = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1,
                              &integrity, NULL) == SQLITE_OK);
    assert(sqlite3_step(integrity) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(integrity, 0), "ok") == 0);
    sqlite3_finalize(integrity);
    sqlite3_close(db);

    r2_log_shutdown();
    puts("Life Log legacy schema migration preserved old events and accepted new writable events.");
    return 0;
}
