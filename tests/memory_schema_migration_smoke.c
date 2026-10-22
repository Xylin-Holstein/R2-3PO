#define _GNU_SOURCE
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef R2_ROOT
#error R2_ROOT must be set to an isolated test directory
#endif

int main(void)
{
    if (mkdir(R2_ROOT, 0755) != 0 && errno != EEXIST) {
        perror("mkdir R2_ROOT");
        return 1;
    }
    if (mkdir(R2_HOME, 0755) != 0 && errno != EEXIST) {
        perror("mkdir R2_HOME");
        return 1;
    }

    sqlite3 *legacy = NULL;
    assert(sqlite3_open(DB_PATH, &legacy) == SQLITE_OK);
    char *error = NULL;
    int rc = sqlite3_exec(legacy,
        "CREATE TABLE memories ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "content TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "timestamp TEXT NOT NULL);"
        "INSERT INTO memories(content,source,timestamp) VALUES("
        "'Mother is an important relationship',"
        "'relationship',"
        "'2025-10-01T12:34:56');",
        NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could not create legacy fixture: %s\n",
                error ? error : sqlite3_errmsg(legacy));
        sqlite3_free(error);
        sqlite3_close(legacy);
        return 1;
    }
    sqlite3_close(legacy);

    assert(init_db() == 0);

    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT memory,category,created_at,updated_at FROM memories WHERE id=1",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *memory = (const char *)sqlite3_column_text(st, 0);
    const char *category = (const char *)sqlite3_column_text(st, 1);
    const char *created = (const char *)sqlite3_column_text(st, 2);
    const char *updated = (const char *)sqlite3_column_text(st, 3);
    assert(memory && !strcmp(memory, "Mother is an important relationship"));
    assert(category && !strcmp(category, "relationship"));
    assert(created && !strcmp(created, "2025-10-01T12:34:56"));
    assert(updated && !strcmp(updated, "2025-10-01T12:34:56"));
    sqlite3_finalize(st);

    /* An additive migration leaves legacy NOT NULL columns in place. A real
       save must still satisfy those old constraints and keep old readers useful. */
    assert(save_memory("New memory after legacy migration", "relationship") == 0);
    assert(sqlite3_prepare_v2(db,
        "SELECT memory,category,created_at,updated_at,content,source,timestamp "
        "FROM memories WHERE memory='New memory after legacy migration'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 0),
                   "New memory after legacy migration"));
    assert(!strcmp((const char *)sqlite3_column_text(st, 1), "relationship"));
    assert(sqlite3_column_text(st, 2) && sqlite3_column_text(st, 3));
    assert(!strcmp((const char *)sqlite3_column_text(st, 4),
                   "New memory after legacy migration"));
    assert(!strcmp((const char *)sqlite3_column_text(st, 5), "relationship"));
    assert(sqlite3_column_text(st, 6) &&
           strlen((const char *)sqlite3_column_text(st, 6)) > 0);
    sqlite3_finalize(st);

    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    puts("Legacy memory schema migration preserved content, category, and timestamps.");
    return 0;
}
