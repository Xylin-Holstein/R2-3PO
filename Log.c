/*
 * ============================================================
 * R2-3PO LIFE LOG
 * ============================================================
 *
 * Log.c maintains a durable chronological record of R2's actual
 * operation, observations, system state, and milestones.
 *
 * This is NOT the private reflective diary:
 *
 *   r2_diary.c  -> first-person reflection and diary writing
 *   Log.c       -> factual event chronology and system history
 *   r2.c        -> existing conversation, memory, and model core
 *
 * The Life Log uses the existing R2 SQLite database but owns a
 * separate connection and creates only r2_log_* tables. It does
 * not alter memories or diary_entries.
 *
 * Build with the existing R2 modules and SQLite:
 *   gcc ... r2.c shell.c r2_diary.c Log.c Ears.c Eyes.c ...
 * ============================================================
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "Log.h"
#include "r2.h"
#include "r2_diary.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/types.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#include <sqlite3.h>
#include <json-c/json.h>

#define R2_LOG_MAX_TEXT       65536
#define R2_LOG_DEFAULT_LIMIT  25
#define R2_LOG_MAX_LIMIT      500

static sqlite3 *log_db = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_initialized = 0;
static int64_t current_session_id = 0;
static struct timespec process_start_time;
static int process_clock_ready = 0;

static pthread_mutex_t text_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int text_log_warning_emitted = 0;

/*
 * Append every structured Life Log event to a daily human-readable
 * mirror as well as SQLite. The database remains canonical; the text
 * mirror is deliberately best-effort so disk permission issues never
 * cause a successfully recorded event to be reported as failed.
 *
 * File format: /home/x/R2_Home/R2_Log/Log[YYYY-MM-DD].txt
 */
static const char *category_name(R2LogCategory category);

static void append_daily_text_log(const char *local_time,
                                  int64_t event_id,
                                  R2LogCategory category,
                                  const char *event_type,
                                  const char *summary,
                                  const char *details,
                                  const char *source)
{
    char directory[512];
    char filename[640];
    char date[11];
    int written;

    if (!local_time || strlen(local_time) < 10)
        return;

    memcpy(date, local_time, 10);
    date[10] = '\0';

    written = snprintf(directory, sizeof(directory), "%s/R2_Log", R2_ROOT);
    if (written < 0 || (size_t)written >= sizeof(directory))
        return;

    if (mkdir(directory, 0750) != 0 && errno != EEXIST)
        goto warning;

    written = snprintf(filename, sizeof(filename),
                       "%s/Log[%s].txt", directory, date);
    if (written < 0 || (size_t)written >= sizeof(filename))
        return;

    pthread_mutex_lock(&text_log_lock);
    FILE *file = fopen(filename, "a");
    if (!file) {
        pthread_mutex_unlock(&text_log_lock);
        goto warning;
    }

    fprintf(file,
            "[%s] [#%lld] [%s/%s] %s\n",
            local_time, (long long)event_id, category_name(category),
            event_type ? event_type : "event",
            summary ? summary : "");
    if (source && *source)
        fprintf(file, "Source: %s\n", source);
    if (details && *details)
        fprintf(file, "%s%s", details,
                details[strlen(details) - 1] == '\n' ? "" : "\n");
    fputc('\n', file);

    if (fflush(file) != 0 || ferror(file)) {
        fclose(file);
        pthread_mutex_unlock(&text_log_lock);
        goto warning;
    }
    fclose(file);
    pthread_mutex_unlock(&text_log_lock);
    return;

warning:
    pthread_mutex_lock(&text_log_lock);
    if (!text_log_warning_emitted) {
        text_log_warning_emitted = 1;
        fprintf(stderr,
                "[R2 Life Log] Warning: daily text mirror could not be "
                "written under %s (check directory permissions).\n",
                R2_ROOT);
    }
    pthread_mutex_unlock(&text_log_lock);
}

/* ------------------------------------------------------------
 * SMALL INTERNAL HELPERS
 * ------------------------------------------------------------ */

static const char *category_name(R2LogCategory category)
{
    switch (category) {
        case R2_LOG_SYSTEM:     return "system";
        case R2_LOG_LIFECYCLE:  return "lifecycle";
        case R2_LOG_SENSORY:    return "sensory";
        case R2_LOG_THINKING:   return "thinking";
        case R2_LOG_FILESYSTEM: return "filesystem";
        case R2_LOG_MEDIA:      return "media";
        case R2_LOG_WORLD:      return "world";
        case R2_LOG_MILESTONE:  return "milestone";
        case R2_LOG_MEMORY:     return "memory";
        case R2_LOG_EXPERIMENT: return "experiment";
        case R2_LOG_ERROR:      return "error";
        case R2_LOG_EMOTION:    return "emotion";
        case R2_LOG_BELIEF:     return "belief";
        case R2_LOG_CONTINUITY: return "continuity";
        case R2_LOG_CONVERSATION:return "conversation";
        case R2_LOG_HYPOTHETICAL:return "hypothetical";
        default:                return "other";
    }
}

static void timestamp_pair(char *utc, size_t utc_size,
                           char *local, size_t local_size)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    struct tm tm_local;

    if (utc && utc_size) {
        if (gmtime_r(&now, &tm_utc))
            strftime(utc, utc_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        else
            snprintf(utc, utc_size, "unavailable");
    }

    if (local && local_size) {
        if (localtime_r(&now, &tm_local))
            strftime(local, local_size, "%Y-%m-%d %H:%M:%S %z",
                     &tm_local);
        else
            snprintf(local, local_size, "unavailable");
    }
}

uint64_t r2_log_elapsed_ms(void)
{
    struct timespec now;

    if (!process_clock_ready)
        return 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    int64_t seconds = (int64_t)now.tv_sec -
                      (int64_t)process_start_time.tv_sec;
    int64_t nanoseconds = (int64_t)now.tv_nsec -
                          (int64_t)process_start_time.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000LL;
    }

    if (seconds < 0)
        return 0;

    return (uint64_t)seconds * 1000ULL +
           (uint64_t)nanoseconds / 1000000ULL;
}

static int exec_sql(const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(log_db, sql, NULL, NULL, &error);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Life Log] SQLite error: %s\n",
                error ? error : sqlite3_errmsg(log_db));
        sqlite3_free(error);
        return -1;
    }

    return 0;
}

static int bind_optional_text(sqlite3_stmt *statement, int index,
                              const char *value)
{
    if (value)
        return sqlite3_bind_text(statement, index, value, -1,
                                 SQLITE_TRANSIENT);

    return sqlite3_bind_null(statement, index);
}

static int valid_text(const char *value)
{
    return value && value[0] != '\0';
}

/* Bounded append helper for query results. */
static int append_text(char **buffer, size_t *length, size_t *capacity,
                       const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return -1;
    }

    if ((size_t)needed > R2_LOG_MAX_TEXT - *length) {
        va_end(args);
        return -1;
    }

    size_t required = *length + (size_t)needed + 1;

    if (required > *capacity) {
        size_t next = *capacity ? *capacity : 4096;

        while (next < required) {
            if (next > R2_LOG_MAX_TEXT / 2) {
                next = R2_LOG_MAX_TEXT;
                break;
            }
            next *= 2;
        }

        char *grown = realloc(*buffer, next);
        if (!grown) {
            va_end(args);
            return -1;
        }

        *buffer = grown;
        *capacity = next;
    }

    vsnprintf(*buffer + *length, *capacity - *length, format, args);
    *length += (size_t)needed;
    va_end(args);
    return 0;
}

static int ensure_tables(void)
{
    static const char *schema =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS r2_log_sessions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " started_utc TEXT NOT NULL,"
        " started_local TEXT NOT NULL,"
        " ended_utc TEXT,"
        " ended_local TEXT,"
        " start_monotonic_ms INTEGER NOT NULL,"
        " end_monotonic_ms INTEGER,"
        " process_id INTEGER NOT NULL,"
        " end_reason TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " utc_time TEXT NOT NULL,"
        " local_time TEXT NOT NULL,"
        " monotonic_ms INTEGER NOT NULL,"
        " session_id INTEGER,"
        " category TEXT NOT NULL,"
        " event_type TEXT NOT NULL,"
        " summary TEXT NOT NULL,"
        " details TEXT,"
        " source TEXT,"
        " related_event_id INTEGER,"
        " memory_saved INTEGER NOT NULL DEFAULT 0,"
        " FOREIGN KEY(session_id) REFERENCES r2_log_sessions(id),"
        " FOREIGN KEY(related_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_log_events_time_idx "
        "ON r2_log_events(id);"
        "CREATE INDEX IF NOT EXISTS r2_log_events_category_idx "
        "ON r2_log_events(category, id);"
        "CREATE TABLE IF NOT EXISTS r2_log_milestones ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " milestone_key TEXT NOT NULL UNIQUE,"
        " description TEXT NOT NULL,"
        " first_event_id INTEGER NOT NULL,"
        " recorded_utc TEXT NOT NULL,"
        " FOREIGN KEY(first_event_id) REFERENCES r2_log_events(id)"
        ");"

        "CREATE TABLE IF NOT EXISTS r2_log_conversations ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL UNIQUE,"
        " user_text TEXT NOT NULL,"
        " assistant_text TEXT NOT NULL,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_inner_states ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL,"
        " state_kind TEXT NOT NULL,"
        " description TEXT NOT NULL,"
        " evidence TEXT,"
        " confidence REAL,"
        " origin TEXT,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_log_inner_states_kind_idx "
        "ON r2_log_inner_states(state_kind, id);"
        "CREATE TABLE IF NOT EXISTS r2_log_beliefs ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " belief_key TEXT NOT NULL UNIQUE,"
        " belief TEXT NOT NULL,"
        " evidence TEXT,"
        " confidence REAL,"
        " status TEXT NOT NULL,"
        " origin TEXT,"
        " first_seen_utc TEXT NOT NULL,"
        " last_updated_utc TEXT NOT NULL,"
        " last_event_id INTEGER NOT NULL,"
        " FOREIGN KEY(last_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_continuity ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " item_key TEXT NOT NULL UNIQUE,"
        " item_type TEXT NOT NULL,"
        " title TEXT NOT NULL,"
        " description TEXT,"
        " status TEXT NOT NULL,"
        " next_action TEXT,"
        " origin TEXT,"
        " first_seen_utc TEXT NOT NULL,"
        " last_updated_utc TEXT NOT NULL,"
        " completed_utc TEXT,"
        " last_event_id INTEGER NOT NULL,"
        " FOREIGN KEY(last_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS r2_log_continuity_status_idx "
        "ON r2_log_continuity(status, item_type);"
        "CREATE TABLE IF NOT EXISTS r2_log_links ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " from_event_id INTEGER NOT NULL,"
        " to_event_id INTEGER NOT NULL,"
        " relationship TEXT NOT NULL,"
        " notes TEXT,"
        " created_utc TEXT NOT NULL,"
        " UNIQUE(from_event_id, to_event_id, relationship),"
        " FOREIGN KEY(from_event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(to_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_hypotheticals ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL UNIQUE,"
        " scenario TEXT NOT NULL,"
        " assumptions TEXT,"
        " predicted_outcome TEXT,"
        " conclusion TEXT,"
        " origin TEXT,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_system_snapshots ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL,"
        " hostname TEXT,"
        " operating_system TEXT,"
        " kernel_release TEXT,"
        " machine TEXT,"
        " process_id INTEGER NOT NULL,"
        " process_elapsed_ms INTEGER NOT NULL,"
        " system_uptime_seconds INTEGER,"
        " total_ram_bytes INTEGER,"
        " free_ram_bytes INTEGER,"
        " workspace_total_bytes INTEGER,"
        " workspace_free_bytes INTEGER,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");";

    if (exec_sql(schema) != 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------
 * INITIALIZATION / SHUTDOWN
 * ------------------------------------------------------------ */

int r2_log_init(void)
{
    int rc;

    pthread_mutex_lock(&log_lock);

    if (log_initialized) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &process_start_time) == 0)
        process_clock_ready = 1;

    rc = sqlite3_open_v2(
        R2_DIARY_DATABASE,
        &log_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
        SQLITE_OPEN_FULLMUTEX,
        NULL
    );

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Life Log] Cannot open %s: %s\n",
                R2_DIARY_DATABASE,
                log_db ? sqlite3_errmsg(log_db) : "unknown error");

        if (log_db)
            sqlite3_close(log_db);

        log_db = NULL;
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    sqlite3_busy_timeout(log_db, 5000);
    sqlite3_exec(log_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", NULL, NULL, NULL);

    if (ensure_tables() != 0) {
        sqlite3_close(log_db);
        log_db = NULL;
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    log_initialized = 1;
    pthread_mutex_unlock(&log_lock);

    if (r2_log_session_start() != 0) {
        fprintf(stderr,
                "[R2 Life Log] Warning: database initialized, "
                "but session start could not be recorded.\n");
    }

    r2_log_event(
        R2_LOG_LIFECYCLE,
        "life_log_initialized",
        "R2 Life Log initialized.",
        "The chronological event log is available.",
        "Log.c"
    );

    r2_log_milestone(
        "life_log_first_initialized",
        "R2 Life Log was initialized for the first time.",
        "First initialization is recorded once and is not replaced "
        "by later starts."
    );

    fprintf(stderr, "[R2 Life Log] Initialized: %s\n",
            R2_DIARY_DATABASE);
    return 0;
}

void r2_log_shutdown(void)
{
    sqlite3 *to_close = NULL;

    if (!r2_log_is_initialized())
        return;

    r2_log_session_end("normal_shutdown");

    pthread_mutex_lock(&log_lock);

    if (log_db) {
        sqlite3_wal_checkpoint_v2(log_db, NULL,
                                  SQLITE_CHECKPOINT_PASSIVE,
                                  NULL, NULL);
        to_close = log_db;
        log_db = NULL;
    }

    log_initialized = 0;
    current_session_id = 0;

    pthread_mutex_unlock(&log_lock);

    if (to_close)
        sqlite3_close(to_close);

    fprintf(stderr, "[R2 Life Log] Shutdown.\n");
}

int r2_log_is_initialized(void)
{
    int result;

    pthread_mutex_lock(&log_lock);
    result = log_initialized && log_db != NULL;
    pthread_mutex_unlock(&log_lock);

    return result;
}

/* ------------------------------------------------------------
 * EVENT RECORDING
 * ------------------------------------------------------------ */

int64_t r2_log_event(
    R2LogCategory category,
    const char *event_type,
    const char *summary,
    const char *details,
    const char *source
)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int64_t inserted_id = -1;
    int rc;

    if (!valid_text(event_type) || !valid_text(summary))
        return -1;

    if (strlen(event_type) > 128 || strlen(summary) > 8192 ||
        (details && strlen(details) > R2_LOG_MAX_TEXT) ||
        (source && strlen(source) > 512)) {
        errno = E2BIG;
        return -1;
    }

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_events "
        "(utc_time, local_time, monotonic_ms, session_id, category, "
        "event_type, summary, details, source) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3,
                           (sqlite3_int64)r2_log_elapsed_ms());

        if (current_session_id > 0)
            sqlite3_bind_int64(statement, 4, current_session_id);
        else
            sqlite3_bind_null(statement, 4);

        sqlite3_bind_text(statement, 5, category_name(category),
                          -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 6, event_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 7, summary, -1, SQLITE_TRANSIENT);
        bind_optional_text(statement, 8, details);
        bind_optional_text(statement, 9, source);

        rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE)
            inserted_id = sqlite3_last_insert_rowid(log_db);
        else
            fprintf(stderr, "[R2 Life Log] Insert failed: %s\n",
                    sqlite3_errmsg(log_db));
    } else {
        fprintf(stderr, "[R2 Life Log] Prepare failed: %s\n",
                sqlite3_errmsg(log_db));
    }

    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);

    if (inserted_id > 0)
        append_daily_text_log(local, inserted_id, category, event_type,
                              summary, details, source);

    return inserted_id;
}

int64_t r2_log_event_with_memory(
    R2LogCategory category,
    const char *event_type,
    const char *summary,
    const char *details,
    const char *source,
    int save_as_memory
)
{
    int64_t event_id = r2_log_event(
        category, event_type, summary, details, source
    );

    if (event_id < 0 || !save_as_memory)
        return event_id;

    /*
     * Save only a concise index/pointer in ordinary memory.
     * The full factual event remains in r2_log_events.
     * This call happens outside log_lock to avoid lock coupling.
     */
    char memory[2048];
    snprintf(memory, sizeof(memory),
             "Life Log event %" PRId64 ": [%s/%s] %s",
             event_id, category_name(category), event_type, summary);

    if (r2_save_memory(memory, "experience") == 0) {
        sqlite3_stmt *statement = NULL;

        pthread_mutex_lock(&log_lock);
        if (log_initialized && log_db &&
            sqlite3_prepare_v2(
                log_db,
                "UPDATE r2_log_events SET memory_saved=1 WHERE id=?;",
                -1, &statement, NULL
            ) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, event_id);
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);
    }

    return event_id;
}

/*
 * Return the Unix epoch of the most recent persisted conversation turn.
 * Returns 0 when no prior turn is available. This lets a new process/session
 * calculate elapsed time without treating old chat as the current live chat.
 */
int64_t r2_log_last_conversation_epoch(void)
{
    sqlite3_stmt *statement = NULL;
    int64_t result = 0;

    pthread_mutex_lock(&log_lock);
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "SELECT CAST(strftime('%s', utc_time) AS INTEGER) "
            "FROM r2_log_events WHERE event_type='conversation_turn' "
            "ORDER BY id DESC LIMIT 1;",
            -1, &statement, NULL) == SQLITE_OK) {
        if (sqlite3_step(statement) == SQLITE_ROW)
            result = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);
    return result > 0 ? result : 0;
}

/*
 * Store the exact visible conversation turn in both the searchable
 * chronological event stream and the dedicated conversation table.
 * The dedicated table is for durable full-text continuity; the event
 * details make the same turn discoverable through r2_log_search().
 */
int64_t r2_log_conversation_turn(const char *user_text,
                                const char *assistant_text)
{
    if (!valid_text(user_text) || !valid_text(assistant_text))
        return -1;

    size_t user_len = strlen(user_text);
    size_t assistant_len = strlen(assistant_text);
    const size_t max_each = R2_LOG_MAX_TEXT / 2 - 128;
    if (user_len > max_each) user_len = max_each;
    if (assistant_len > max_each) assistant_len = max_each;

    size_t details_size = user_len + assistant_len + 128;
    char *details = malloc(details_size);
    if (!details) return -1;

    snprintf(details, details_size,
             "USER SAID:\n%.*s\nR2 REPLIED:\n%.*s",
             (int)user_len, user_text,
             (int)assistant_len, assistant_text);

    char summary[1024];
    size_t preview = user_len;
    if (preview > 800) preview = 800;
    snprintf(summary, sizeof(summary),
             "Conversation turn: %.*s",
             (int)preview, user_text);
    int64_t event_id = r2_log_event(
        R2_LOG_CONVERSATION, "conversation_turn",
        summary, details, "r2_talk");
    free(details);
    if (event_id < 0) return -1;

    sqlite3_stmt *statement = NULL;
    int rc;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return event_id; /* Event details still preserve the turn. */
    }
    rc = sqlite3_prepare_v2(
        log_db,
        "INSERT OR REPLACE INTO r2_log_conversations "
        "(event_id,user_text,assistant_text) VALUES(?,?,?);",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, event_id);
        sqlite3_bind_text(statement, 2, user_text, (int)user_len, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, assistant_text, (int)assistant_len, SQLITE_TRANSIENT);
        rc = sqlite3_step(statement);
    }
    if (rc != SQLITE_DONE)
        fprintf(stderr, "[R2 Life Log] Conversation archive insert failed: %s\n",
                sqlite3_errmsg(log_db));
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);

    /* The searchable event remains authoritative if the archive insert fails. */
    return event_id;
}

/* ------------------------------------------------------------
 * SESSION / FIRSTS
 * ------------------------------------------------------------ */

int r2_log_session_start(void)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int rc;
    int64_t id = -1;

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_sessions "
        "(started_utc, started_local, start_monotonic_ms, process_id) "
        "VALUES (?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3,
                           (sqlite3_int64)r2_log_elapsed_ms());
        sqlite3_bind_int(statement, 4, (int)getpid());

        if (sqlite3_step(statement) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(log_db);
    }

    sqlite3_finalize(statement);

    if (id > 0)
        current_session_id = id;

    pthread_mutex_unlock(&log_lock);

    if (id > 0) {
        r2_log_event(R2_LOG_LIFECYCLE, "session_started",
                     "R2 process session started.",
                     "A new session record was created.", "Log.c");
        return 0;
    }

    return -1;
}

int r2_log_session_end(const char *reason)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int rc;
    int64_t session_id;

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db || current_session_id <= 0) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }

    session_id = current_session_id;

    rc = sqlite3_prepare_v2(
        log_db,
        "UPDATE r2_log_sessions SET ended_utc=?, ended_local=?, "
        "end_monotonic_ms=?, end_reason=? WHERE id=? AND ended_utc IS NULL;",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3,
                           (sqlite3_int64)r2_log_elapsed_ms());
        bind_optional_text(statement, 4, reason);
        sqlite3_bind_int64(statement, 5, session_id);
        rc = sqlite3_step(statement);
    }

    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);

    if (rc != SQLITE_DONE && rc != SQLITE_OK)
        return -1;

    /* Keep the active session ID until its final event is written. */
    r2_log_event(R2_LOG_LIFECYCLE, "session_ended",
                 "R2 process session ended.",
                 reason ? reason : "No shutdown reason supplied.",
                 "Log.c");

    pthread_mutex_lock(&log_lock);
    current_session_id = 0;
    pthread_mutex_unlock(&log_lock);

    return 0;
}

int64_t r2_log_milestone(
    const char *milestone_key,
    const char *description,
    const char *details
)
{
    sqlite3_stmt *statement = NULL;
    int64_t event_id;
    int64_t milestone_id = -1;
    char utc[40];
    char local[48];

    if (!valid_text(milestone_key) || !valid_text(description))
        return -1;

    /*
     * Check before creating the event: repeat boots must not create
     * duplicate "first" milestones.
     */
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    int exists = 0;
    if (sqlite3_prepare_v2(log_db,
            "SELECT 1 FROM r2_log_milestones WHERE milestone_key=?;",
            -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, milestone_key, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(statement) == SQLITE_ROW;
    }
    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);
    if (exists)
        return 0;

    event_id = r2_log_event(
        R2_LOG_MILESTONE, "milestone",
        description, details, milestone_key
    );
    if (event_id < 0)
        return -1;

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    int rc = sqlite3_prepare_v2(
        log_db,
        "INSERT OR IGNORE INTO r2_log_milestones "
        "(milestone_key, description, first_event_id, recorded_utc) "
        "VALUES (?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, milestone_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, description, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, event_id);
        sqlite3_bind_text(statement, 4, utc, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(statement) == SQLITE_DONE &&
            sqlite3_changes(log_db) > 0) {
            milestone_id = sqlite3_last_insert_rowid(log_db);
        }
    }

    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);

    return milestone_id >= 0 ? event_id : 0;
}

/* ------------------------------------------------------------
 * SPECIALIZED EVENT HELPERS
 * ------------------------------------------------------------ */

int64_t r2_log_sensory(const char *sense, const char *observation,
                      const char *details, const char *source)
{
    if (!valid_text(sense) || !valid_text(observation))
        return -1;

    return r2_log_event(R2_LOG_SENSORY, sense, observation,
                        details, source ? source : "sensory");
}

int64_t r2_log_thinking(const char *thought_kind, const char *summary,
                        const char *details)
{
    if (!valid_text(thought_kind) || !valid_text(summary))
        return -1;

    return r2_log_event(R2_LOG_THINKING, thought_kind, summary,
                        details, "internal_thinking");
}

int64_t r2_log_file_event(const char *action, const char *path,
                          const char *result, const char *details)
{
    char summary[2048];

    if (!valid_text(action) || !valid_text(path) || !valid_text(result))
        return -1;

    snprintf(summary, sizeof(summary), "%s: %s (%s)", action, path, result);

    return r2_log_event(R2_LOG_FILESYSTEM, action, summary,
                        details, "filesystem");
}

int64_t r2_log_media_event(const char *action, const char *media_type,
                           const char *path_or_identifier,
                           const char *details)
{
    char summary[2048];

    if (!valid_text(action) || !valid_text(media_type) ||
        !valid_text(path_or_identifier))
        return -1;

    snprintf(summary, sizeof(summary), "%s %s: %s",
             action, media_type, path_or_identifier);

    return r2_log_event(R2_LOG_MEDIA, action, summary,
                        details, "media");
}

int64_t r2_log_world_event(const char *object_or_device,
                           const char *event, const char *result,
                           const char *details)
{
    char summary[2048];

    if (!valid_text(object_or_device) || !valid_text(event) ||
        !valid_text(result))
        return -1;

    snprintf(summary, sizeof(summary), "%s: %s — %s",
             object_or_device, event, result);

    return r2_log_event(R2_LOG_WORLD, event, summary,
                        details, "world");
}

/* ------------------------------------------------------------
 * SYSTEM SELF-AWARENESS SNAPSHOT
 * ------------------------------------------------------------ */

int r2_log_system_snapshot(const char *reason)
{
    struct utsname uts;
    struct statvfs fs;
    char details[8192];
    char summary[512];
    char hostname[256] = "unavailable";
    const char *os_name = "unavailable";
    const char *kernel = "unavailable";
    const char *machine = "unavailable";
    uint64_t total_ram = 0;
    uint64_t free_ram = 0;
    int64_t system_uptime = -1;
    uint64_t disk_total = 0;
    uint64_t disk_free = 0;
    double load_average[3] = {0.0, 0.0, 0.0};
    int have_load_average = 0;
    int have_uts = 0;
    int have_fs = 0;
    int64_t event_id;

    if (uname(&uts) == 0) {
        have_uts = 1;
        os_name = uts.sysname;
        kernel = uts.release;
        machine = uts.machine;
    }

    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
        snprintf(hostname, sizeof(hostname), "unavailable");
    hostname[sizeof(hostname) - 1] = '\0';

#ifdef __linux__
    {
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            system_uptime = (int64_t)info.uptime;
            total_ram = (uint64_t)info.totalram * info.mem_unit;
            free_ram = (uint64_t)info.freeram * info.mem_unit;
        }
    }
#endif

    if (getloadavg(load_average, 3) == 3)
        have_load_average = 1;

    if (statvfs(R2_ROOT, &fs) == 0) {
        have_fs = 1;
        disk_total = (uint64_t)fs.f_blocks * fs.f_frsize;
        disk_free = (uint64_t)fs.f_bavail * fs.f_frsize;
    }

    snprintf(summary, sizeof(summary),
             "R2 system state snapshot%s%s",
             reason && *reason ? ": " : "",
             reason && *reason ? reason : "");

    snprintf(details, sizeof(details),
             "hostname=%s\n"
             "os=%s\n"
             "kernel=%s\n"
             "machine=%s\n"
             "process_id=%ld\n"
             "process_elapsed_ms=%" PRIu64 "\n"
             "system_uptime_seconds=%" PRId64 "\n"
             "total_ram_bytes=%" PRIu64 "\n"
             "free_ram_bytes=%" PRIu64 "\n"
             "load_average_1m=%.2f\n"
             "load_average_5m=%.2f\n"
             "load_average_15m=%.2f\n"
             "load_average_available=%s\n"
             "workspace=%s\n"
             "workspace_disk_total_bytes=%" PRIu64 "\n"
             "workspace_disk_available_bytes=%" PRIu64 "\n"
             "uname_available=%s\n"
             "workspace_statvfs_available=%s\n",
             hostname, os_name, kernel, machine, (long)getpid(),
             r2_log_elapsed_ms(), system_uptime, total_ram, free_ram,
             load_average[0], load_average[1], load_average[2],
             have_load_average ? "yes" : "no",
             R2_ROOT, disk_total, disk_free,
             have_uts ? "yes" : "no", have_fs ? "yes" : "no");

    event_id = r2_log_event(R2_LOG_SYSTEM, "system_snapshot",
                            summary, details, "system_self_awareness");

    if (event_id < 0)
        return -1;

    pthread_mutex_lock(&log_lock);

    if (log_initialized && log_db) {
        sqlite3_stmt *statement = NULL;

        int rc = sqlite3_prepare_v2(
            log_db,
            "INSERT INTO r2_log_system_snapshots "
            "(event_id, hostname, operating_system, kernel_release, machine, "
            "process_id, process_elapsed_ms, system_uptime_seconds, "
            "total_ram_bytes, free_ram_bytes, workspace_total_bytes, "
            "workspace_free_bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            -1, &statement, NULL
        );

        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, event_id);
            sqlite3_bind_text(statement, 2, hostname, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, os_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, kernel, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 5, machine, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 6, (int)getpid());
            sqlite3_bind_int64(statement, 7,
                               (sqlite3_int64)r2_log_elapsed_ms());

            if (system_uptime >= 0)
                sqlite3_bind_int64(statement, 8, system_uptime);
            else
                sqlite3_bind_null(statement, 8);

            if (total_ram)
                sqlite3_bind_int64(statement, 9, (sqlite3_int64)total_ram);
            else
                sqlite3_bind_null(statement, 9);

            if (free_ram)
                sqlite3_bind_int64(statement, 10, (sqlite3_int64)free_ram);
            else
                sqlite3_bind_null(statement, 10);

            if (have_fs) {
                sqlite3_bind_int64(statement, 11, (sqlite3_int64)disk_total);
                sqlite3_bind_int64(statement, 12, (sqlite3_int64)disk_free);
            } else {
                sqlite3_bind_null(statement, 11);
                sqlite3_bind_null(statement, 12);
            }

            rc = sqlite3_step(statement);
        }

        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);

        return rc == SQLITE_DONE ? 0 : -1;
    }

    pthread_mutex_unlock(&log_lock);
    return -1;
}


/* ------------------------------------------------------------
 * STRUCTURED SELF-STATE / BELIEF / CONTINUITY / RELATIONSHIPS
 * These functions are declared in Log.h and are used by r2.c.
 * Each structured record also gets a chronological event.
 * ------------------------------------------------------------ */

int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes)
{
    if (from_event_id <= 0 || to_event_id <= 0 || !valid_text(relationship))
        return -1;
    char utc[40], local[48];
    timestamp_pair(utc, sizeof(utc), local, sizeof(local));
    (void)local;

    pthread_mutex_lock(&log_lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "INSERT OR IGNORE INTO r2_log_links "
            "(from_event_id,to_event_id,relationship,notes,created_utc) "
            "VALUES(?,?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, from_event_id);
        sqlite3_bind_int64(st, 2, to_event_id);
        sqlite3_bind_text(st, 3, relationship, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 4, notes);
        sqlite3_bind_text(st, 5, utc, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc;
}

int64_t r2_log_inner_state(const char *state_kind,
                           const char *description,
                           const char *evidence,
                           double confidence,
                           const char *origin)
{
    if (!valid_text(state_kind) || !valid_text(description)) return -1;
    char details[8192];
    snprintf(details, sizeof(details), "evidence=%s\nconfidence=%.4f",
             evidence ? evidence : "(not provided)", confidence);
    int64_t event_id = r2_log_event(R2_LOG_EMOTION, "inner_state",
                                    description, details, origin);
    if (event_id <= 0) return event_id;

    pthread_mutex_lock(&log_lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "INSERT INTO r2_log_inner_states "
            "(event_id,state_kind,description,evidence,confidence,origin) "
            "VALUES(?,?,?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, event_id);
        sqlite3_bind_text(st, 2, state_kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, description, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 4, evidence);
        if (confidence < 0.0) sqlite3_bind_null(st, 5);
        else sqlite3_bind_double(st, 5, confidence > 1.0 ? 1.0 : confidence);
        bind_optional_text(st, 6, origin);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == 0 ? event_id : -1;
}

int64_t r2_log_belief(const char *belief_key,
                      const char *belief,
                      const char *evidence,
                      double confidence,
                      const char *status,
                      const char *origin)
{
    if (!valid_text(belief_key) || !valid_text(belief) || !valid_text(status))
        return -1;
    char details[8192];
    snprintf(details, sizeof(details), "key=%s\nstatus=%s\nconfidence=%.4f\nevidence=%s",
             belief_key, status, confidence, evidence ? evidence : "(not provided)");
    int64_t event_id = r2_log_event(R2_LOG_BELIEF, "belief_updated",
                                    belief, details, origin);
    if (event_id <= 0) return event_id;

    char utc[40], local[48];
    timestamp_pair(utc, sizeof(utc), local, sizeof(local));
    pthread_mutex_lock(&log_lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "INSERT INTO r2_log_beliefs "
            "(belief_key,belief,evidence,confidence,status,origin,first_seen_utc,last_updated_utc,last_event_id) "
            "VALUES(?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(belief_key) DO UPDATE SET "
            "belief=excluded.belief,evidence=excluded.evidence,confidence=excluded.confidence,"
            "status=excluded.status,origin=excluded.origin,last_updated_utc=excluded.last_updated_utc,"
            "last_event_id=excluded.last_event_id;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, belief_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, belief, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 3, evidence);
        if (confidence < 0.0) sqlite3_bind_null(st, 4);
        else sqlite3_bind_double(st, 4, confidence > 1.0 ? 1.0 : confidence);
        sqlite3_bind_text(st, 5, status, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 6, origin);
        sqlite3_bind_text(st, 7, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 9, event_id);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == 0 ? event_id : -1;
}

int64_t r2_log_continuity(const char *item_key,
                          const char *item_type,
                          const char *title,
                          const char *description,
                          const char *status,
                          const char *next_action,
                          const char *origin)
{
    if (!valid_text(item_key) || !valid_text(item_type) ||
        !valid_text(title) || !valid_text(status)) return -1;
    char details[8192];
    snprintf(details, sizeof(details), "key=%s\ntype=%s\nstatus=%s\nnext_action=%s\ndescription=%s",
             item_key, item_type, status, next_action ? next_action : "(none)",
             description ? description : "(none)");
    int64_t event_id = r2_log_event(R2_LOG_CONTINUITY, "continuity_updated",
                                    title, details, origin);
    if (event_id <= 0) return event_id;

    char utc[40], local[48];
    timestamp_pair(utc, sizeof(utc), local, sizeof(local));
    pthread_mutex_lock(&log_lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "INSERT INTO r2_log_continuity "
            "(item_key,item_type,title,description,status,next_action,origin,first_seen_utc,last_updated_utc,completed_utc,last_event_id) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(item_key) DO UPDATE SET "
            "item_type=excluded.item_type,title=excluded.title,description=excluded.description,"
            "status=excluded.status,next_action=excluded.next_action,origin=excluded.origin,"
            "last_updated_utc=excluded.last_updated_utc,"
            "completed_utc=CASE WHEN lower(excluded.status) IN ('complete','completed','done') "
            "THEN excluded.last_updated_utc ELSE r2_log_continuity.completed_utc END,"
            "last_event_id=excluded.last_event_id;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, item_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, item_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 4, description);
        sqlite3_bind_text(st, 5, status, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 6, next_action);
        bind_optional_text(st, 7, origin);
        sqlite3_bind_text(st, 8, utc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, utc, -1, SQLITE_TRANSIENT);
        if (!strcasecmp(status, "complete") || !strcasecmp(status, "completed") ||
            !strcasecmp(status, "done"))
            sqlite3_bind_text(st, 10, utc, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 10);
        sqlite3_bind_int64(st, 11, event_id);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == 0 ? event_id : -1;
}

int64_t r2_log_hypothetical(const char *scenario,
                            const char *assumptions,
                            const char *predicted_outcome,
                            const char *conclusion,
                            const char *origin)
{
    if (!valid_text(scenario)) return -1;
    size_t cap = 8192;
    char *details = malloc(cap);
    if (!details) return -1;
    snprintf(details, cap, "ASSUMPTIONS:\n%s\nPREDICTED OUTCOME:\n%s\nCONCLUSION:\n%s",
             assumptions ? assumptions : "(not provided)",
             predicted_outcome ? predicted_outcome : "(not provided)",
             conclusion ? conclusion : "(not provided)");
    int64_t event_id = r2_log_event(R2_LOG_HYPOTHETICAL, "hypothetical_scenario",
                                    scenario, details, origin);
    free(details);
    if (event_id <= 0) return event_id;

    pthread_mutex_lock(&log_lock);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (log_initialized && log_db &&
        sqlite3_prepare_v2(log_db,
            "INSERT INTO r2_log_hypotheticals "
            "(event_id,scenario,assumptions,predicted_outcome,conclusion,origin) "
            "VALUES(?,?,?,?,?,?);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, event_id);
        sqlite3_bind_text(st, 2, scenario, -1, SQLITE_TRANSIENT);
        bind_optional_text(st, 3, assumptions);
        bind_optional_text(st, 4, predicted_outcome);
        bind_optional_text(st, 5, conclusion);
        bind_optional_text(st, 6, origin);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == 0 ? event_id : -1;
}

char *r2_log_status_report(void)
{
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return strdup("R2 Life Log is not initialized.\n");
    }
    const char *sql =
        "SELECT "
        "(SELECT count(*) FROM r2_log_events),"
        "(SELECT count(*) FROM r2_log_sessions),"
        "(SELECT count(*) FROM r2_log_conversations),"
        "(SELECT count(*) FROM r2_log_beliefs),"
        "(SELECT count(*) FROM r2_log_continuity),"
        "(SELECT count(*) FROM r2_log_events WHERE memory_saved=1);";
    sqlite3_stmt *st = NULL;
    long long events = 0, sessions = 0, turns = 0, beliefs = 0, continuity = 0, saved = 0;
    if (sqlite3_prepare_v2(log_db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        events = sqlite3_column_int64(st, 0);
        sessions = sqlite3_column_int64(st, 1);
        turns = sqlite3_column_int64(st, 2);
        beliefs = sqlite3_column_int64(st, 3);
        continuity = sqlite3_column_int64(st, 4);
        saved = sqlite3_column_int64(st, 5);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);

    char *report = malloc(512);
    if (!report) return NULL;
    snprintf(report, 512,
             "R2 LIFE LOG STATUS\n"
             "Events recorded: %lld\nSessions recorded: %lld\n"
             "Conversation turns archived: %lld\nCurrent belief records: %lld\n"
             "Continuity records: %lld\nEvents linked to saved memories: %lld\n",
             events, sessions, turns, beliefs, continuity, saved);
    return report;
}

/* ------------------------------------------------------------
 * SEARCH / RECENT HISTORY
 * ------------------------------------------------------------ */

static int normalize_limit(int limit)
{
    if (limit <= 0)
        return R2_LOG_DEFAULT_LIMIT;
    if (limit > R2_LOG_MAX_LIMIT)
        return R2_LOG_MAX_LIMIT;
    return limit;
}

static char *query_events(const char *query, int limit)
{
    sqlite3_stmt *statement = NULL;
    char *output = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int rc;

    limit = normalize_limit(limit);

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    const char *sql_recent =
        "SELECT id, utc_time, local_time, category, event_type, summary, "
        "details, source, memory_saved FROM r2_log_events "
        "ORDER BY id DESC LIMIT ?;";

    const char *sql_search =
        "SELECT id, utc_time, local_time, category, event_type, summary, "
        "details, source, memory_saved FROM r2_log_events "
        "WHERE summary LIKE ? ESCAPE '\\' OR details LIKE ? ESCAPE '\\' "
        "OR event_type LIKE ? ESCAPE '\\' OR category LIKE ? ESCAPE '\\' "
        "OR source LIKE ? ESCAPE '\\' "
        "ORDER BY id DESC LIMIT ?;";

    rc = sqlite3_prepare_v2(
        log_db, query ? sql_search : sql_recent, -1, &statement, NULL
    );

    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    if (query) {
        size_t qlen = strlen(query);
        char *pattern = malloc(qlen * 2 + 3);

        if (!pattern) {
            sqlite3_finalize(statement);
            pthread_mutex_unlock(&log_lock);
            return NULL;
        }

        size_t p = 0;
        pattern[p++] = '%';

        for (size_t i = 0; i < qlen; ++i) {
            if (query[i] == '%' || query[i] == '_' || query[i] == '\\')
                pattern[p++] = '\\';
            pattern[p++] = query[i];
        }

        pattern[p++] = '%';
        pattern[p] = '\0';

        for (int i = 1; i <= 5; ++i)
            sqlite3_bind_text(statement, i, pattern, -1, SQLITE_TRANSIENT);

        sqlite3_bind_int(statement, 6, limit);
        free(pattern);
    } else {
        sqlite3_bind_int(statement, 1, limit);
    }

    output = malloc(4096);
    if (!output) {
        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    output[0] = '\0';
    capacity = 4096;

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(statement, 0);
        const unsigned char *utc = sqlite3_column_text(statement, 1);
        const unsigned char *local = sqlite3_column_text(statement, 2);
        const unsigned char *category = sqlite3_column_text(statement, 3);
        const unsigned char *type = sqlite3_column_text(statement, 4);
        const unsigned char *summary = sqlite3_column_text(statement, 5);
        const unsigned char *details = sqlite3_column_text(statement, 6);
        const unsigned char *source = sqlite3_column_text(statement, 7);
        int memory_saved = sqlite3_column_int(statement, 8);

        if (append_text(&output, &length, &capacity,
                        "[%" PRId64 "] %s | %s | %s/%s\n"
                        "  Summary: %s\n"
                        "  Source: %s | Memory indexed: %s\n",
                        id,
                        utc ? (const char *)utc : "unknown UTC time",
                        local ? (const char *)local : "unknown local time",
                        category ? (const char *)category : "unknown",
                        type ? (const char *)type : "unknown",
                        summary ? (const char *)summary : "",
                        source ? (const char *)source : "unspecified",
                        memory_saved ? "yes" : "no") != 0) {
            free(output);
            output = NULL;
            break;
        }

        if (details && *(const char *)details) {
            if (append_text(&output, &length, &capacity,
                            "  Details: %s\n", (const char *)details) != 0) {
                free(output);
                output = NULL;
                break;
            }
        }

        if (append_text(&output, &length, &capacity, "\n") != 0) {
            free(output);
            output = NULL;
            break;
        }
    }

    if (output && length == 0) {
        const char *empty = query
            ? "No Life Log events matched that search.\n"
            : "The Life Log contains no events yet.\n";
        snprintf(output, capacity, "%s", empty);
    }

    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);
    return output;
}

char *r2_log_recent(int limit)
{
    return query_events(NULL, limit);
}

char *r2_log_conversation_recent_json(int limit)
{
    if (limit < 1) limit = 30;
    if (limit > 200) limit = 200;

    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    const char *sql =
        "SELECT id, utc_time, local_time, user_text, assistant_text "
        "FROM ("
        " SELECT c.id AS id, e.utc_time AS utc_time, "
        " e.local_time AS local_time, c.user_text AS user_text, "
        " c.assistant_text AS assistant_text "
        " FROM r2_log_conversations c "
        " JOIN r2_log_events e ON e.id = c.event_id "
        " ORDER BY c.id DESC LIMIT ?1"
        ") ORDER BY id ASC";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(log_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }
    sqlite3_bind_int(stmt, 1, limit);

    struct json_object *array = json_object_new_array();
    if (!array) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct json_object *turn = json_object_new_object();
        if (!turn) continue;
        json_object_object_add(turn, "id",
            json_object_new_int64(sqlite3_column_int64(stmt, 0)));
        const unsigned char *utc = sqlite3_column_text(stmt, 1);
        const unsigned char *local = sqlite3_column_text(stmt, 2);
        const unsigned char *user = sqlite3_column_text(stmt, 3);
        const unsigned char *assistant = sqlite3_column_text(stmt, 4);
        json_object_object_add(turn, "utc_time",
            json_object_new_string(utc ? (const char *)utc : ""));
        json_object_object_add(turn, "local_time",
            json_object_new_string(local ? (const char *)local : ""));
        json_object_object_add(turn, "user",
            json_object_new_string(user ? (const char *)user : ""));
        json_object_object_add(turn, "assistant",
            json_object_new_string(assistant ? (const char *)assistant : ""));
        json_object_array_add(array, turn);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&log_lock);

    if (step != SQLITE_DONE) {
        json_object_put(array);
        return NULL;
    }

    const char *serialized = json_object_to_json_string_ext(
        array, JSON_C_TO_STRING_PLAIN);
    char *result = serialized ? strdup(serialized) : NULL;
    json_object_put(array);
    return result;
}

char *r2_log_search(const char *query, int limit)
{
    if (!valid_text(query))
        return r2_log_recent(limit);

    return query_events(query, limit);
}
