#define _POSIX_C_SOURCE 200809L
#include "AlternateSelf.h"
#include "Log.h"
#include "r2_diary.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int ensure_log_schema(void)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS r2_log_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "category INTEGER NOT NULL,"
        "event_type TEXT NOT NULL,"
        "summary TEXT NOT NULL,"
        "details TEXT,"
        "source TEXT);"
        "INSERT INTO r2_log_events(category,event_type,summary,details,source) "
        "SELECT 0,'test_evidence','Existing evidence','Fixture row','test' "
        "WHERE NOT EXISTS (SELECT 1 FROM r2_log_events);";
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        fprintf(stderr, "test database setup failed: %s\n", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static int64_t insert_log_event(R2LogCategory category, const char *type,
                                const char *summary, const char *details,
                                const char *source)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "INSERT INTO r2_log_events(category,event_type,summary,details,source) "
        "VALUES(?,?,?,?,?);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(st, 1, category);
        sqlite3_bind_text(st, 2, type ? type : "test", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, summary ? summary : "", -1, SQLITE_TRANSIENT);
        if (details) sqlite3_bind_text(st, 4, details, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 4);
        if (source) sqlite3_bind_text(st, 5, source, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 5);
        rc = sqlite3_step(st);
    }
    int64_t id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return id;
}

/* Test doubles isolate AlternateSelf.c while preserving its real SQLite behavior. */
int64_t r2_log_event(R2LogCategory category, const char *event_type,
                     const char *summary, const char *details, const char *source)
{
    return insert_log_event(category, event_type, summary, details, source);
}

int64_t r2_log_hypothetical(const char *scenario, const char *assumptions,
                            const char *predicted_outcome, const char *conclusion,
                            const char *origin)
{
    (void)assumptions;
    (void)predicted_outcome;
    return insert_log_event(R2_LOG_HYPOTHETICAL, "hypothetical", scenario,
                            conclusion, origin);
}

int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes)
{
    (void)from_event_id;
    (void)to_event_id;
    (void)relationship;
    (void)notes;
    return 0;
}

static int check(int condition, const char *message)
{
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main(void)
{
    const char *directory = "/tmp/r2-altself-smoke";
    const char *database = R2_DIARY_DATABASE;
    (void)mkdir(directory, 0700);
    unlink(database);
    char sidecar[512];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", database); unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", database); unlink(sidecar);

    int passed = 0;
    int64_t evidence_id = 0, first = 0, second = 0, imported_event = 0;
    char *view = NULL;

    if (!check(ensure_log_schema() == 0, "create fixture Life Log table")) goto done;
    sqlite3 *fixture = NULL;
    if (sqlite3_open(database, &fixture) != SQLITE_OK) {
        if (fixture) sqlite3_close(fixture);
        goto done;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fixture, "SELECT id FROM r2_log_events ORDER BY id LIMIT 1;",
                           -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        evidence_id = sqlite3_column_int64(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(fixture);
    if (!check(evidence_id > 0, "fixture evidence ID exists")) goto done;
    if (!check(r2_altself_init() == 0, "initialize Alternate-Self Lab schema")) goto done;

    first = r2_altself_create("Look left", "What if I looked left?",
                              "I have not looked yet.", "I might see an object.",
                              "This is only a prediction.", evidence_id);
    if (!check(first > 0, "create a hypothetical branch with evidence")) goto done;
    second = r2_altself_create("Look right", "What if I looked right?",
                               "I have not looked yet.", "I might see something else.",
                               "Compare only after actual observation.", evidence_id);
    if (!check(second > 0 && second != first, "create a second branch")) goto done;

    view = r2_altself_show(first);
    if (!check(view && strstr(view, "What if I looked left?") && strstr(view, "active"),
               "show branch and status")) goto done;
    free(view); view = NULL;

    view = r2_altself_compare(first, second);
    if (!check(view && strstr(view, "Look left") && strstr(view, "Look right"),
               "compare arbitrary branches")) goto done;
    free(view); view = NULL;

    if (!check(r2_altself_retain_hypothesis(first) == 0,
               "retain branch as a hypothesis")) goto done;
    view = r2_altself_show(first);
    if (!check(view && strstr(view, "retained_hypothesis"),
               "retained branch remains explicitly hypothetical")) goto done;
    free(view); view = NULL;

    if (!check(r2_altself_discard(second) == 0, "discard branch without deleting history")) goto done;
    view = r2_altself_show(second);
    if (!check(view && strstr(view, "discarded"), "discard status is persisted")) goto done;
    free(view); view = NULL;

    imported_event = r2_log_hypothetical("Would I still like X if I knew Y?",
                                         "Y is hypothetical.", "My preference might change.",
                                         "Prediction only.", "smoke test");
    if (!check(imported_event > 0, "create source hypothetical Life Log event")) goto done;
    int64_t imported1 = r2_altself_import_hypothesis(
        "Would I still like X if I knew Y?", "Y is hypothetical.",
        "My preference might change.", "Prediction only.", imported_event, evidence_id);
    int64_t imported2 = r2_altself_import_hypothesis(
        "Would I still like X if I knew Y?", "Y is hypothetical.",
        "My preference might change.", "Prediction only.", imported_event, evidence_id);
    if (!check(imported1 > 0 && imported1 == imported2,
               "importing the same hypothetical is idempotent")) goto done;

    view = r2_altself_list(20);
    if (!check(view && strstr(view, "Look left") && strstr(view, "Look right") &&
               strstr(view, "Would I still like X if I knew Y?"),
               "list all saved what-if branches")) goto done;
    free(view); view = NULL;

    passed = 1;
done:
    free(view);
    r2_altself_shutdown();
    unlink(database);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", database); unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", database); unlink(sidecar);
    (void)rmdir(directory);
    if (passed) puts("Alternate-Self Lab smoke tests passed.");
    return passed ? 0 : 1;
}
