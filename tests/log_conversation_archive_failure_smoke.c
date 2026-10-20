#include "Log.h"
#include "r2_diary.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Isolate the Life Log test from the optional Sensory Journal database. */
void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id;
    (void)category;
    (void)event_type;
    (void)source;
}

static int query_count(sqlite3 *db, const char *sql,
                       sqlite3_int64 bind_id, sqlite3_int64 *count)
{
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK && bind_id > 0)
        rc = sqlite3_bind_int64(statement, 1, bind_id);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(statement, 0);
        rc = SQLITE_OK;
    }
    sqlite3_finalize(statement);
    return rc == SQLITE_OK ? 0 : -1;
}

int main(void)
{
    if (r2_log_init() != 0) {
        fprintf(stderr, "FAIL: Life Log initialization failed.\n");
        return 1;
    }

    int64_t first_turn = r2_log_conversation_turn(
        "FIRST_PERSISTED_USER_MARKER",
        "FIRST_PERSISTED_REPLY_MARKER");
    int64_t second_turn = r2_log_conversation_turn(
        "SECOND_PERSISTED_USER_MARKER",
        "SECOND_PERSISTED_REPLY_MARKER");
    if (first_turn <= 0 || second_turn <= first_turn) {
        fprintf(stderr, "FAIL: could not persist ordered conversation fixtures.\\n");
        r2_log_shutdown();
        return 1;
    }

    /* Restart the Life Log to prove recent turns are restored from SQLite,
       rather than relying only on the in-memory message array. */
    r2_log_shutdown();
    if (r2_log_init() != 0) {
        fprintf(stderr, "FAIL: Life Log did not reopen after conversation writes.\\n");
        return 1;
    }
    char *recent_context = r2_log_recent_conversation_context(3000U);
    const char *first_marker = recent_context
        ? strstr(recent_context, "FIRST_PERSISTED_USER_MARKER") : NULL;
    const char *second_marker = recent_context
        ? strstr(recent_context, "SECOND_PERSISTED_USER_MARKER") : NULL;
    if (!recent_context || strlen(recent_context) > 3000U ||
        !first_marker || !second_marker || first_marker >= second_marker) {
        fprintf(stderr, "FAIL: recent persisted conversation was missing, unbounded, or out of order.\\n");
        free(recent_context);
        r2_log_shutdown();
        return 1;
    }
    free(recent_context);

    sqlite3 *inject_db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &inject_db) != SQLITE_OK) {
        fprintf(stderr, "FAIL: could not open test database connection.\n");
        if (inject_db) sqlite3_close(inject_db);
        r2_log_shutdown();
        return 1;
    }

    char *error = NULL;
    int rc = sqlite3_exec(
        inject_db,
        "CREATE TRIGGER fail_conversation_archive "
        "BEFORE INSERT ON r2_log_conversations "
        "BEGIN SELECT RAISE(ABORT, 'injected archive write failure'); END;",
        NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "FAIL: could not install failure trigger: %s\n",
                error ? error : sqlite3_errmsg(inject_db));
        sqlite3_free(error);
        sqlite3_close(inject_db);
        r2_log_shutdown();
        return 1;
    }
    sqlite3_close(inject_db);

    int64_t event_id = r2_log_conversation_turn(
        "What did we just discuss?",
        "We were discussing the recent conversation.");
    if (event_id <= 0) {
        fprintf(stderr, "FAIL: primary searchable conversation event was lost.\n");
        r2_log_shutdown();
        return 1;
    }

    sqlite3 *check_db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &check_db) != SQLITE_OK) {
        fprintf(stderr, "FAIL: could not reopen test database.\n");
        if (check_db) sqlite3_close(check_db);
        r2_log_shutdown();
        return 1;
    }

    sqlite3_int64 count = 0;
    if (query_count(check_db,
        "SELECT count(*) FROM r2_log_events "
        "WHERE id=?1 AND event_type='conversation_turn';",
        event_id, &count) != 0 || count != 1) {
        fprintf(stderr, "FAIL: searchable primary conversation event is missing.\n");
        sqlite3_close(check_db);
        r2_log_shutdown();
        return 1;
    }

    if (query_count(check_db,
        "SELECT count(*) FROM r2_log_conversations WHERE event_id=?1;",
        event_id, &count) != 0 || count != 0) {
        fprintf(stderr, "FAIL: failure injection did not block the archive row.\n");
        sqlite3_close(check_db);
        r2_log_shutdown();
        return 1;
    }

    if (query_count(check_db,
        "SELECT count(*) FROM r2_log_events "
        "WHERE event_type='conversation_archive_insert_failed' "
        "AND details LIKE '%injected archive write failure%' "
        "AND instr(details, 'conversation_event_id=' || ?1) > 0;",
        event_id, &count) != 0 || count < 1) {
        fprintf(stderr, "FAIL: archive failure was not recorded as a searchable event.\n");
        sqlite3_close(check_db);
        r2_log_shutdown();
        return 1;
    }

    sqlite3_close(check_db);
    r2_log_shutdown();
    puts("Conversation archive failure regression passed.");
    return 0;
}
