#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "SensoryJournal.h"
#include "Log.h"
#include "r2_diary.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *sj_db;
static pthread_mutex_t sj_lock = PTHREAD_MUTEX_INITIALIZER;
static int sj_ready;

static int sj_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(sj_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Sensory Journal] SQLite error: %s\n",
                err ? err : (sj_db ? sqlite3_errmsg(sj_db) : "no database"));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int r2_sj_init(void)
{
    pthread_mutex_lock(&sj_lock);
    if (sj_ready) {
        pthread_mutex_unlock(&sj_lock);
        return 0;
    }
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &sj_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            NULL) != SQLITE_OK) {
        fprintf(stderr, "[R2 Sensory Journal] Cannot open %s\n", R2_DIARY_DATABASE);
        if (sj_db) sqlite3_close(sj_db);
        sj_db = NULL;
        pthread_mutex_unlock(&sj_lock);
        return -1;
    }
    sqlite3_busy_timeout(sj_db, 5000);
    sqlite3_exec(sj_db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;",
                 NULL, NULL, NULL);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_sensory_journal ("
        " event_id INTEGER PRIMARY KEY,"
        " journal_kind TEXT NOT NULL,"
        " indexed_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_sensory_journal_kind_idx "
        "ON r2_sensory_journal(journal_kind,event_id);"
        "CREATE TABLE IF NOT EXISTS r2_sensory_journal_diary_links ("
        " event_id INTEGER PRIMARY KEY,"
        " diary_entry_id INTEGER NOT NULL,"
        " linked_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(diary_entry_id) REFERENCES diary_entries(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_sensory_journal_diary_entry_idx "
        "ON r2_sensory_journal_diary_links(diary_entry_id);"
        "INSERT OR IGNORE INTO r2_sensory_journal_diary_links(event_id,diary_entry_id) "
        "SELECT e.id,d.id FROM r2_log_events e "
        "JOIN diary_entries d ON d.entry=e.details "
        "WHERE e.category='thinking' "
        "AND e.event_type IN ('diary_entry_written','reflection_completed');"
        "INSERT OR IGNORE INTO r2_sensory_journal(event_id,journal_kind) "
        "SELECT id,category FROM r2_log_events "
        "WHERE category IN ('sensory','thinking','conversation');";
    if (sj_exec(schema) != 0) {
        sqlite3_close(sj_db);
        sj_db = NULL;
        pthread_mutex_unlock(&sj_lock);
        return -1;
    }
    sj_ready = 1;
    pthread_mutex_unlock(&sj_lock);
    fprintf(stderr, "[R2 Sensory Journal] Initialized.\n");
    return 0;
}

void r2_sj_shutdown(void)
{
    pthread_mutex_lock(&sj_lock);
    if (sj_db) sqlite3_close(sj_db);
    sj_db = NULL;
    sj_ready = 0;
    pthread_mutex_unlock(&sj_lock);
}

void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    if (event_id <= 0 || !category || !event_type) return;
    const char *kind = NULL;
    if (!strcmp(category, "sensory")) kind = "sensory";
    else if (!strcmp(category, "thinking")) kind = "thinking";
    else if (!strcmp(category, "conversation")) kind = "conversation";
    else return;

    pthread_mutex_lock(&sj_lock);
    if (!sj_ready || !sj_db) {
        pthread_mutex_unlock(&sj_lock);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sj_db,
            "INSERT OR IGNORE INTO r2_sensory_journal(event_id,journal_kind) VALUES(?,?);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, event_id);
        sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE)
            fprintf(stderr, "[R2 Sensory Journal] Index failed: %s\n", sqlite3_errmsg(sj_db));
    }
    sqlite3_finalize(st);

    /* Link diary-writing events to the original diary row without copying it. */
    if (!strcmp(category, "thinking") &&
        (!strcmp(event_type, "diary_entry_written") ||
         !strcmp(event_type, "reflection_completed"))) {
        sqlite3_stmt *link = NULL;
        int link_rc = sqlite3_prepare_v2(sj_db,
            "INSERT OR REPLACE INTO r2_sensory_journal_diary_links(event_id,diary_entry_id) "
            "SELECT e.id,d.id FROM r2_log_events e "
            "JOIN diary_entries d ON d.entry=e.details "
            "WHERE e.id=? ORDER BY d.id DESC LIMIT 1;",
            -1, &link, NULL);
        if (link_rc == SQLITE_OK) {
            sqlite3_bind_int64(link, 1, event_id);
            if (sqlite3_step(link) != SQLITE_DONE)
                fprintf(stderr, "[R2 Sensory Journal] Diary link failed: %s\\n",
                        sqlite3_errmsg(sj_db));
        } else {
            fprintf(stderr, "[R2 Sensory Journal] Diary link query failed: %s\\n",
                    sqlite3_errmsg(sj_db));
        }
        sqlite3_finalize(link);
    }
    pthread_mutex_unlock(&sj_lock);
    (void)source;
}

int64_t r2_sj_note(const char *note, const char *details)
{
    if (!note || !*note) return -1;
    return r2_log_thinking("sensory_journal_note", note,
                           details ? details : "Explicit journal note recorded.");
}

static char *sj_query(const char *query, int limit)
{
    if (limit < 1) limit = 25;
    if (limit > 200) limit = 200;
    pthread_mutex_lock(&sj_lock);
    if (!sj_ready || !sj_db) {
        pthread_mutex_unlock(&sj_lock);
        return NULL;
    }

    const char *sql_recent =
        "SELECT e.id,e.utc_time,e.local_time,j.journal_kind,e.event_type,e.summary,"
        "e.details,e.source,d.diary_entry_id FROM r2_sensory_journal j "
        "JOIN r2_log_events e ON e.id=j.event_id "
        "LEFT JOIN r2_sensory_journal_diary_links d ON d.event_id=e.id "
        "ORDER BY e.id DESC LIMIT ?;";
    const char *sql_search =
        "SELECT e.id,e.utc_time,e.local_time,j.journal_kind,e.event_type,e.summary,"
        "e.details,e.source,d.diary_entry_id FROM r2_sensory_journal j "
        "JOIN r2_log_events e ON e.id=j.event_id "
        "LEFT JOIN r2_sensory_journal_diary_links d ON d.event_id=e.id "
        "WHERE e.summary LIKE ? OR e.details LIKE ? OR e.event_type LIKE ? "
        "OR j.journal_kind LIKE ? OR e.source LIKE ? ORDER BY e.id DESC LIMIT ?;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sj_db, query ? sql_search : sql_recent, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&sj_lock);
        return NULL;
    }
    if (query) {
        size_t n = strlen(query);
        if (n > 1024) n = 1024;
        char *pat = malloc(n + 3);
        if (!pat) { sqlite3_finalize(st); pthread_mutex_unlock(&sj_lock); return NULL; }
        pat[0] = '%'; memcpy(pat + 1, query, n); pat[n + 1] = '%'; pat[n + 2] = '\0';
        for (int i = 1; i <= 5; ++i) sqlite3_bind_text(st, i, pat, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 6, limit);
        free(pat);
    } else sqlite3_bind_int(st, 1, limit);

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { sqlite3_finalize(st); pthread_mutex_unlock(&sj_lock); return NULL; }
    out[0] = '\0';
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *utc = sqlite3_column_text(st, 1);
        const unsigned char *local = sqlite3_column_text(st, 2);
        const unsigned char *kind = sqlite3_column_text(st, 3);
        const unsigned char *type = sqlite3_column_text(st, 4);
        const unsigned char *summary = sqlite3_column_text(st, 5);
        const unsigned char *details = sqlite3_column_text(st, 6);
        const unsigned char *source = sqlite3_column_text(st, 7);
        const unsigned char *diary_id = sqlite3_column_text(st, 8);
        int needed = snprintf(NULL, 0,
            "[%lld] %s | %s | %s/%s\n  %s\n  Source: %s\n  Diary link: %s\n  %s\n\n",
            (long long)sqlite3_column_int64(st, 0),
            utc ? (const char *)utc : "unknown UTC",
            local ? (const char *)local : "unknown local",
            kind ? (const char *)kind : "unknown",
            type ? (const char *)type : "unknown",
            summary ? (const char *)summary : "",
            source ? (const char *)source : "unspecified",
            diary_id ? (const char *)diary_id : "none",
            details ? (const char *)details : "");
        if (needed < 0 || len + (size_t)needed + 1 > 2 * 1024 * 1024) break;
        if (len + (size_t)needed + 1 > cap) {
            size_t next = cap;
            while (next < len + (size_t)needed + 1) next *= 2;
            char *grown = realloc(out, next);
            if (!grown) { free(out); out = NULL; break; }
            out = grown; cap = next;
        }
        snprintf(out + len, cap - len,
            "[%lld] %s | %s | %s/%s\n  %s\n  Source: %s\n  Diary link: %s\n  %s\n\n",
            (long long)sqlite3_column_int64(st, 0),
            utc ? (const char *)utc : "unknown UTC",
            local ? (const char *)local : "unknown local",
            kind ? (const char *)kind : "unknown",
            type ? (const char *)type : "unknown",
            summary ? (const char *)summary : "",
            source ? (const char *)source : "unspecified",
            diary_id ? (const char *)diary_id : "none",
            details ? (const char *)details : "");
        len += (size_t)needed;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&sj_lock);
    if (out && len == 0)
        snprintf(out, cap, "%s", query ? "No Sensory Journal entries matched.\n" :
                                      "The Sensory Journal contains no entries yet.\n");
    return out;
}

char *r2_sj_recent(int limit) { return sj_query(NULL, limit); }
char *r2_sj_search(const char *query, int limit)
{
    if (!query || !*query) return r2_sj_recent(limit);
    return sj_query(query, limit);
}

long r2_sj_count(void)
{
    pthread_mutex_lock(&sj_lock);
    if (!sj_ready || !sj_db) {
        pthread_mutex_unlock(&sj_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    long count = -1;
    if (sqlite3_prepare_v2(sj_db,
            "SELECT COUNT(*) FROM r2_sensory_journal;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        count = (long)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&sj_lock);
    return count;
}
