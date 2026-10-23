#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define R2_HOME "/tmp/r2-reward-legacy-schema-smoke"
#include "../Reward.c"

/* Isolate reward persistence from the rest of the robot during this schema test. */
int r2_log_is_initialized(void) { return 0; }
int64_t r2_log_event_with_memory(R2LogCategory category, const char *event_type,
    const char *summary, const char *details, const char *source, int save_as_memory)
{
    (void)category; (void)event_type; (void)summary; (void)details;
    (void)source; (void)save_as_memory;
    return -1;
}
int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category;
    return 0;
}
int r2_addiction_record_choice(const char *target, const char *target_type,
    int enjoyment_0_100, const char *source, const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details;
    return 0;
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

static void remove_if_present(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        perror(path);
        exit(1);
    }
}

int main(void)
{
    char sidecar[sizeof(REWARD_DB_PATH) + 8];
    remove_if_present(REWARD_DB_PATH);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", REWARD_DB_PATH);
    remove_if_present(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", REWARD_DB_PATH);
    remove_if_present(sidecar);
    if (rmdir(R2_HOME) != 0 && errno != ENOENT) {
        perror("rmdir reward test root");
        return 1;
    }
    assert(mkdir(R2_HOME, 0755) == 0);

    sqlite3 *legacy = NULL;
    assert(sqlite3_open(REWARD_DB_PATH, &legacy) == SQLITE_OK);
    assert(sqlite3_exec(legacy,
        "CREATE TABLE reward_totals (id INTEGER PRIMARY KEY CHECK(id=1), points INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO reward_totals(id,points) VALUES(1,4);"
        "CREATE TABLE reward_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,target TEXT NOT NULL,source TEXT NOT NULL,"
        "points INTEGER NOT NULL CHECK(points BETWEEN -7 AND 5 AND points<>0),"
        "reason TEXT NOT NULL,occurred_at INTEGER NOT NULL,modifier INTEGER NOT NULL,expires_at INTEGER NOT NULL);"
        "INSERT INTO reward_events(target,source,points,reason,occurred_at,modifier,expires_at)"
        " VALUES('old target','old source',1,'preserve this row',100,2,1900);"
        "CREATE TABLE reward_once_keys (target TEXT NOT NULL,source TEXT NOT NULL,PRIMARY KEY(target,source));"
        "INSERT INTO reward_once_keys(target,source) VALUES('old target','old key');",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(legacy);

    /* Opening an older ledger must add missing fields without replacing history. */
    assert(r2_reward_init() == 0);
    assert(r2_reward_apply("new target", "new source", 1,
                           "write after additive migration", 0) == 0);
    assert(r2_reward_apply_once("once target", "once source", 2,
                                "legacy idempotency schema", 0) == 0);
    assert(r2_reward_apply_once("once target", "once source", 2,
                                "legacy idempotency schema", 0) == 1);

    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 7);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_events WHERE target='old target' AND reason='preserve this row'") == 1);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_events") == 3);
    assert(reward_column_exists("reward_totals", "updated_at"));
    assert(reward_column_exists("reward_events", "log_event_id"));
    assert(reward_column_exists("reward_events", "memory_indexed"));
    assert(reward_column_exists("reward_once_keys", "created_at"));
    assert(scalar(reward_db, "SELECT updated_at>0 FROM reward_totals WHERE id=1") == 1);
    assert(scalar(reward_db, "SELECT created_at>0 FROM reward_once_keys WHERE target='old target' AND source='old key'") == 1);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_once_keys WHERE target='old target' AND source='old key'") == 1);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_once_keys WHERE target='once target' AND source='once source'") == 1);

    r2_reward_shutdown();
    puts("Reward ledger additive migration preserved legacy rows and allowed writable/idempotent operations.");
    return 0;
}
