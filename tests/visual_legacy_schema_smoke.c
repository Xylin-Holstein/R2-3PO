/* Include the production implementation so this test exercises its real
   legacy-schema migration and insert helper against SQLite. */
#include "../Visual.c"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

/* These inference/logging dependencies are not exercised by this database
   migration test; keep the test local and independent of an Ollama server. */
int r2_ollama_context_tokens(void) { return 8192; }
int r2_ollama_chat_url(char *out, size_t out_cap)
{
    if (!out || out_cap < sizeof("http://127.0.0.1:11434")) return -1;
    snprintf(out, out_cap, "%s", "http://127.0.0.1:11434");
    return 0;
}
int r2_ollama_vision_request_begin(void) { return 1; }
void r2_ollama_vision_request_end(void) {}
int r2_ollama_vision_request_should_abort(void) { return 0; }
int64_t r2_log_sensory(const char *sense, const char *observation,
                       const char *details, const char *source)
{
    (void)sense; (void)observation; (void)details; (void)source;
    return 1;
}

int main(void)
{
    char path[] = "/tmp/r2-visual-legacy-smoke-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(sqlite3_open(path, &visual_db) == SQLITE_OK);

    assert(sqlite3_exec(visual_db,
        "CREATE TABLE r2_visual_experiences ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_at TEXT NOT NULL,"
        "timestamp TEXT NOT NULL,"
        "file_path TEXT NOT NULL,"
        "path TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "description TEXT NOT NULL);"
        "INSERT INTO r2_visual_experiences(created_at,timestamp,file_path,path,source,description) "
        "VALUES('2020-01-02T03:04:05Z','2020-01-02T03:04:05Z',"
        "'/legacy/old.jpg','/legacy/old.jpg','legacy-camera','old frame');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(visual_migrate_schema() == SQLITE_OK);

    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(visual_db,
        "SELECT created_utc,image_path FROM r2_visual_experiences WHERE description='old frame'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "2020-01-02T03:04:05Z") == 0);
    assert(strcmp((const char *)sqlite3_column_text(st, 1), "/legacy/old.jpg") == 0);
    sqlite3_finalize(st);

    assert(visual_insert_compat("2026-10-16T07:00:00Z", "Eyes",
        "/new/frame.jpg", "qwen3.5:9b", NULL, "new frame",
        640, 480, 7, 123456, 0) == SQLITE_DONE);

    assert(sqlite3_prepare_v2(visual_db,
        "SELECT created_utc,image_path,created_at,timestamp,file_path,path "
        "FROM r2_visual_experiences WHERE description='new frame'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    for (int i = 0; i < 6; ++i) {
        const char *expected = (i == 1 || i == 4 || i == 5)
            ? "/new/frame.jpg" : "2026-10-16T07:00:00Z";
        assert(strcmp((const char *)sqlite3_column_text(st, i), expected) == 0);
    }
    sqlite3_finalize(st);
    assert(sqlite3_close(visual_db) == SQLITE_OK);
    visual_db = NULL;
    assert(unlink(path) == 0);
    puts("Legacy visual schema migration preserves old rows and supports new writes.");
    return 0;
}
