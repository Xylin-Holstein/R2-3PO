#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "Reality.h"
#include "r2_diary.h"
#include "Log.h"
#include "r2.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REALITY_MAX_TEXT 4096
#define REALITY_MAX_OUTPUT 32768

static sqlite3 *reality_db = NULL;
static pthread_mutex_t reality_lock = PTHREAD_MUTEX_INITIALIZER;
static int reality_ready = 0;

/* Safe defaults: elapsed world time advances continuously; hunger reaches
 * 100 after 24 hours without a meal, and the 72-hour mark is explicitly
 * described as prolonged starvation rather than silently resetting needs. */
static const double HUNGER_PER_SECOND = 100.0 / 86400.0;

static int exec_sql(const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(reality_db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Reality] SQLite: %s\n", error ? error : "unknown error");
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
    }
    return -1;
}

static int make_room_dirs(void)
{
    char room[1024], shelf[1100], box[1100], pockets[1100], wallet[1100], toy_box[1100];
    snprintf(room, sizeof(room), "%s/room", R2_ROOT);
    snprintf(shelf, sizeof(shelf), "%s/shelf", room);
    snprintf(box, sizeof(box), "%s/box", room);
    snprintf(pockets, sizeof(pockets), "%s/pockets", room);
    snprintf(wallet, sizeof(wallet), "%s/wallet", room);
    snprintf(toy_box, sizeof(toy_box), "%s/toy_box", room);
    if (mkdir_one(room) || mkdir_one(shelf) || mkdir_one(box) ||
        mkdir_one(pockets) || mkdir_one(wallet) || mkdir_one(toy_box)) {
        fprintf(stderr, "[R2 Reality] Could not create room/shelf/box directories under %s\n", R2_ROOT);
        return -1;
    }
    char food_xml[1200];
    snprintf(food_xml, sizeof(food_xml), "%s/food_metrics.xml", room);
    if (access(food_xml, F_OK) != 0) {
        FILE *fp = fopen(food_xml, "w");
        if (fp) {
            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<foods>\n"
                  "  <!-- Add one food per line. fullness is 0..100; energy is optional. -->\n"
                  "  <!-- Example: <food name=\"burger\" fullness=\"100\" energy=\"10\" /> -->\n"
                  "</foods>\n", fp);
            fclose(fp);
        }
    }
    return 0;
}

static void bridge_event(const char *type, const char *summary, const char *details,
                         int remember, int diary)
{
    /* Life Log is the factual chronology; persistent memory gets a searchable
       summary/pointer; the diary receives only a short factual note on major
       world changes, never a duplicate of the whole database. */
    if (r2_log_is_initialized())
        r2_log_event_with_memory(R2_LOG_WORLD, type, summary, details,
                                 "Reality.c", remember);
    else if (remember)
        r2_save_memory(summary, "world");
    if (diary && r2_diary_active())
        r2_diary_write(summary);
}

static int bind_text(sqlite3_stmt *st, int n, const char *s)
{
    return sqlite3_bind_text(st, n, s ? s : "", -1, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}


/* Human-inspectable mirror files make room/shelf/box state visible in the
 * workspace. SQLite remains canonical; pockets/wallet are virtual containers. */
static void item_slug(const char *name, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j + 1 < cap; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (isalnum(ch)) out[j++] = (char)tolower(ch);
        else if ((ch == ' ' || ch == '-' || ch == '_') && j && out[j-1] != '_')
            out[j++] = '_';
    }
    while (j && out[j-1] == '_') --j;
    if (!j && cap > 1) { snprintf(out, cap, "item"); return; }
    out[j] = '\0';
}

static int mirror_path(const char *name, const char *container, char *path, size_t cap)
{
    if (!name || !container || !path) return -1;
    const char *folder = NULL;
    if (!strcmp(container, "room")) folder = "";
    else if (!strcmp(container, "shelf")) folder = "/shelf";
    else if (!strcmp(container, "box")) folder = "/box";
    else if (!strcmp(container, "pockets")) folder = "/pockets";
    else if (!strcmp(container, "wallet")) folder = "/wallet";
    else if (!strcmp(container, "toy box")) folder = "/toy_box";
    else return 1; /* Other named containers remain database-only. */
    char slug[256];
    item_slug(name, slug, sizeof(slug));
    int n = snprintf(path, cap, "%s/room%s/%s.r2item", R2_ROOT, folder, slug);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static void mirror_remove(const char *name, const char *container)
{
    char path[2048];
    if (mirror_path(name, container, path, sizeof(path)) == 0)
        (void)unlink(path);
}

static int mirror_write(const char *name, const char *description,
                        int quantity, const char *container)
{
    char path[2048], temp[2100];
    int p = mirror_path(name, container, path, sizeof(path));
    if (p == 1) return 0;
    if (p != 0) return -1;
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(temp)) return -1;
    FILE *fp = fopen(temp, "w");
    if (!fp) return -1;
    int failed = fprintf(fp, "name=%s\ndescription=%s\nquantity=%d\ncontainer=%s\n",
                         name, description ? description : "", quantity, container) < 0;
    if (fclose(fp) != 0) failed = 1;
    if (!failed && rename(temp, path) != 0) failed = 1;
    if (failed) { unlink(temp); return -1; }
    return 0;
}



static int xml_attribute(const char *line, const char *key, char *out, size_t cap)
{
    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "%s=", key) <= 0) return -1;
    const char *p = strcasestr(line, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"' && *p != '\'') return -1;
    char quote = *p++;
    const char *end = strchr(p, quote);
    if (!end) return -1;
    size_t n = (size_t)(end - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/* Food metrics are data, not hard-coded guesses. Each XML <food> entry
 * supplies a name and fullness value; energy is optional. */
static int food_metric(const char *food, double *fullness, double *energy)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[4096];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        const char *tag = strcasestr(line, "<food");
        const char *comment = strstr(line, "<!--");
        if (!tag || (comment && comment < tag)) continue; /* Ignore XML comments/examples. */
        char name[512] = {0}, full[128] = {0}, en[128] = {0};
        if (xml_attribute(line, "name", name, sizeof(name)) != 0 ||
            strcasecmp(name, food) != 0 ||
            xml_attribute(line, "fullness", full, sizeof(full)) != 0)
            continue;
        char *end = NULL;
        double f = strtod(full, &end);
        if (end == full || *end || f < 0.0 || f > 100.0) continue;
        double e = f * 0.1;
        if (xml_attribute(line, "energy", en, sizeof(en)) == 0) {
            end = NULL;
            double parsed = strtod(en, &end);
            if (end != en && !*end && parsed >= 0.0 && parsed <= 100.0) e = parsed;
        }
        if (fullness) *fullness = f;
        if (energy) *energy = e;
        found = 0;
        break;
    }
    fclose(fp);
    return found;
}



static void food_attributes(const char *food, char *ingredients, size_t ingredients_cap,
                           char *taste, size_t taste_cap)
{
    if (ingredients && ingredients_cap) ingredients[0] = '\0';
    if (taste && taste_cap) taste[0] = '\0';
    if (!food || !*food) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        const char *tag = strcasestr(line, "<food");
        const char *comment = strstr(line, "<!--");
        if (!tag || (comment && comment < tag)) continue;
        char name[512] = {0};
        if (xml_attribute(line, "name", name, sizeof(name)) != 0 ||
            strcasecmp(name, food) != 0) continue;
        if (ingredients && ingredients_cap)
            (void)xml_attribute(line, "ingredients", ingredients, ingredients_cap);
        if (taste && taste_cap)
            (void)xml_attribute(line, "taste", taste, taste_cap);
        break;
    }
    fclose(fp);
}

static char *food_metrics_context(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *fp = fopen(path, "r");
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { if (fp) fclose(fp); return NULL; }
    out[0] = '\0';
    if (!fp) {
        snprintf(out, cap, "No food metric file is available.\n");
        return out;
    }
    char line[4096];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < 100) {
        const char *tag = strcasestr(line, "<food");
        const char *comment = strstr(line, "<!--");
        if (!tag || (comment && comment < tag)) continue;
        char name[512] = {0}, full[128] = {0}, en[128] = {0};
        char ingredients[1024] = {0}, taste[1024] = {0};
        if (xml_attribute(line, "name", name, sizeof(name)) != 0 ||
            xml_attribute(line, "fullness", full, sizeof(full)) != 0)
            continue;
        (void)xml_attribute(line, "ingredients", ingredients, sizeof(ingredients));
        (void)xml_attribute(line, "taste", taste, sizeof(taste));
        char *end = NULL;
        double f = strtod(full, &end);
        if (end == full || *end || f < 0.0 || f > 100.0) continue;
        double e = f * 0.1;
        if (xml_attribute(line, "energy", en, sizeof(en)) == 0) {
            end = NULL;
            double parsed = strtod(en, &end);
            if (end != en && !*end && parsed >= 0.0 && parsed <= 100.0) e = parsed;
        }
        char entry[1024];
        int n = snprintf(entry, sizeof(entry), "%s: fullness %.1f/100, energy bonus %.1f; ingredients: %s; sensory description: %s\n", name, f, e, *ingredients ? ingredients : "not specified", *taste ? taste : "not specified");
        if (n <= 0) continue;
        if (len + (size_t)n + 1 > cap) {
            size_t next = cap * 2;
            char *grown = realloc(out, next);
            if (!grown) break;
            out = grown; cap = next;
        }
        memcpy(out + len, entry, (size_t)n);
        len += (size_t)n; out[len] = '\0';
        count++;
    }
    fclose(fp);
    if (!len) snprintf(out, cap, "No food metrics are configured in room/food_metrics.xml.\n");
    return out;
}

static void update_hunger_locked(double elapsed)
{
    sqlite3_stmt *st = NULL;
    double hunger = 0.0, since_meal = 0.0, sleepiness = 0.0, energy = 100.0;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT hunger, seconds_since_meal, sleepiness, energy FROM r2_reality_self WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        hunger = sqlite3_column_double(st, 0);
        since_meal = sqlite3_column_double(st, 1);
        sleepiness = sqlite3_column_double(st, 2);
        energy = sqlite3_column_double(st, 3);
    }
    if (st) sqlite3_finalize(st);
    hunger += elapsed * HUNGER_PER_SECOND;
    sleepiness += elapsed * HUNGER_PER_SECOND;
    energy -= elapsed * HUNGER_PER_SECOND;
    if (hunger > 100.0) hunger = 100.0;
    if (sleepiness > 100.0) sleepiness = 100.0;
    if (energy < 0.0) energy = 0.0;
    since_meal += elapsed;
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=?, seconds_since_meal=?, sleepiness=?, energy=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hunger);
        sqlite3_bind_double(st, 2, since_meal);
        sqlite3_bind_double(st, 3, sleepiness);
        sqlite3_bind_double(st, 4, energy);
        sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_reality_meta(key,value) VALUES('world_elapsed_seconds',?) ON CONFLICT(key) DO UPDATE SET value=CAST(CAST(value AS INTEGER)+CAST(excluded.value AS INTEGER) AS TEXT)",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)llround(elapsed));
        sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
}



static void sync_room_mirrors(void)
{
    if (!reality_db) return;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT name,description,quantity,container FROM r2_reality_objects ORDER BY id",
        -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 0);
            const unsigned char *description = sqlite3_column_text(st, 1);
            int quantity = sqlite3_column_int(st, 2);
            const unsigned char *container = sqlite3_column_text(st, 3);
            if (name && container)
                (void)mirror_write((const char *)name,
                    description ? (const char *)description : "",
                    quantity, (const char *)container);
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
}

static int legacy_table_exists(const char *name)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT 1 FROM legacy.sqlite_master WHERE type='table' AND name=?",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, name);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    if (st) sqlite3_finalize(st);
    return found;
}

/* One-time, non-destructive migration from older releases where Reality
 * tables lived inside r2_memory.db. Life Log and memory stay in that file. */
static int migrate_legacy_reality(void)
{
    if (access(R2_DIARY_DATABASE, F_OK) != 0) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT value FROM r2_reality_meta WHERE key='legacy_reality_migrated'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        if (st) sqlite3_finalize(st);
        return 0;
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(reality_db, "ATTACH DATABASE ? AS legacy", -1, &st, NULL) != SQLITE_OK) {
        if (st) sqlite3_finalize(st);
        return -1;
    }
    bind_text(st, 1, R2_DIARY_DATABASE);
    int rc = sqlite3_step(st);
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

    const char *tables[] = {
        "r2_reality_containers", "r2_reality_meta", "r2_reality_self",
        "r2_reality_self_facts", "r2_reality_ticks", "r2_reality_objects"
    };
    int result = 0;
    for (size_t i = 0; i < sizeof(tables)/sizeof(tables[0]); ++i) {
        if (!legacy_table_exists(tables[i])) continue;
        char sql[512];
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO main.%s SELECT * FROM legacy.%s",
                 tables[i], tables[i]);
        if (exec_sql(sql) != 0) { result = -1; break; }
    }
    if (result == 0 && legacy_table_exists("r2_reality_self"))
        result = exec_sql("UPDATE main.r2_reality_self SET hunger=(SELECT hunger FROM legacy.r2_reality_self WHERE id=1), seconds_since_meal=(SELECT seconds_since_meal FROM legacy.r2_reality_self WHERE id=1), sleepiness=(SELECT sleepiness FROM legacy.r2_reality_self WHERE id=1), energy=(SELECT energy FROM legacy.r2_reality_self WHERE id=1), last_tick=(SELECT last_tick FROM legacy.r2_reality_self WHERE id=1), updated_at=(SELECT updated_at FROM legacy.r2_reality_self WHERE id=1) WHERE id=1 AND EXISTS(SELECT 1 FROM legacy.r2_reality_self WHERE id=1)");
    if (result == 0 && legacy_table_exists("r2_reality_meta"))
        result = exec_sql("UPDATE main.r2_reality_meta SET value=(SELECT value FROM legacy.r2_reality_meta WHERE legacy.r2_reality_meta.key=main.r2_reality_meta.key) WHERE key IN (SELECT key FROM legacy.r2_reality_meta)");
    if (result == 0)
        result = exec_sql("INSERT INTO r2_reality_meta(key,value) VALUES('legacy_reality_migrated','yes') ON CONFLICT(key) DO UPDATE SET value='yes'");
    (void)sqlite3_exec(reality_db, "DETACH DATABASE legacy", NULL, NULL, NULL);
    if (result == 0)
        fprintf(stderr, "[R2 Reality] Legacy world/needs state migrated into r2_reality.db; r2_memory.db remains intact.\\n");
    return result;
}

int r2_reality_init(void)
{
    pthread_mutex_lock(&reality_lock);
    if (reality_ready) { pthread_mutex_unlock(&reality_lock); return 0; }
    if (make_room_dirs() != 0) { pthread_mutex_unlock(&reality_lock); return -1; }

    int rc = char reality_path[1200];
    snprintf(reality_path, sizeof(reality_path), "%s/r2_reality.db", R2_HOME);
    rc = sqlite3_open_v2(reality_path, &reality_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Reality] Cannot open dedicated database %s: %s\n",
                reality_path, reality_db ? sqlite3_errmsg(reality_db) : "unknown error");
        if (reality_db) sqlite3_close(reality_db);
        reality_db = NULL;
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    sqlite3_busy_timeout(reality_db, 5000);
    sqlite3_exec(reality_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_reality_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO r2_reality_meta(key,value) VALUES('world_elapsed_seconds','0');"
        "CREATE TABLE IF NOT EXISTS r2_reality_item_memory (id INTEGER PRIMARY KEY AUTOINCREMENT, item_name TEXT NOT NULL COLLATE NOCASE, description TEXT, exact_quantity INTEGER, approximate_quantity INTEGER, precision TEXT NOT NULL DEFAULT 'exact' CHECK(precision IN ('exact','approximate','vague')), collected_at INTEGER NOT NULL, last_decay_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS r2_reality_item_memory_age_idx ON r2_reality_item_memory(collected_at);"
        "CREATE TABLE IF NOT EXISTS r2_food_experiences (id INTEGER PRIMARY KEY AUTOINCREMENT, food_name TEXT NOT NULL COLLATE NOCASE, ingredients TEXT, fullness REAL NOT NULL, energy_bonus REAL NOT NULL, satisfaction INTEGER, notes TEXT, eaten_at INTEGER NOT NULL, rated_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS r2_food_experiences_food_idx ON r2_food_experiences(food_name,eaten_at);"
        "CREATE TABLE IF NOT EXISTS r2_food_ingredient_preferences (ingredient TEXT PRIMARY KEY COLLATE NOCASE, satisfaction_sum REAL NOT NULL DEFAULT 0, rating_count INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS r2_food_preferences (food_name TEXT PRIMARY KEY COLLATE NOCASE, satisfaction_sum REAL NOT NULL DEFAULT 0, rating_count INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS r2_food_experience_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO r2_food_experience_meta(key,value) VALUES('schema_version','1');"
        "CREATE TABLE IF NOT EXISTS r2_reality_self (id INTEGER PRIMARY KEY CHECK(id=1), hunger REAL NOT NULL DEFAULT 0, seconds_since_meal REAL NOT NULL DEFAULT 0, sleepiness REAL NOT NULL DEFAULT 0, energy REAL NOT NULL DEFAULT 100, last_tick INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_reality_self(id,hunger,seconds_since_meal,sleepiness,energy,last_tick) VALUES(1,0,0,0,100,strftime('%s','now'));"
        "CREATE TABLE IF NOT EXISTS r2_reality_self_facts (key TEXT PRIMARY KEY, value TEXT NOT NULL, evidence TEXT, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_reality_containers (name TEXT PRIMARY KEY, kind TEXT NOT NULL, description TEXT, parent TEXT, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_reality_objects (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE UNIQUE, description TEXT, quantity INTEGER NOT NULL DEFAULT 1 CHECK(quantity>0), container TEXT NOT NULL DEFAULT 'room', owner TEXT NOT NULL DEFAULT 'R2', created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(container) REFERENCES r2_reality_containers(name));"
        "CREATE INDEX IF NOT EXISTS r2_reality_objects_container_idx ON r2_reality_objects(container);"
        "CREATE TABLE IF NOT EXISTS r2_reality_ticks (id INTEGER PRIMARY KEY AUTOINCREMENT, previous_tick INTEGER NOT NULL, current_tick INTEGER NOT NULL, elapsed_seconds INTEGER NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_reality_containers(name,kind,description,parent) VALUES"
        "('room','room','R2\'s room',''),('shelf','surface','The shelf in R2\'s room','room'),('box','container','The general storage box in R2\'s room','room'),('toy box','container','A toy storage box in R2\'s room','room'),('pockets','inventory','R2\'s pockets','self'),('wallet','inventory','R2\'s wallet','self');";
    if (exec_sql(schema) != 0) {
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (migrate_legacy_reality() != 0) {
        fprintf(stderr, "[R2 Reality] Legacy migration failed; refusing to discard old state.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }

    /* Catch up persistent world time across process restarts. Avoid huge
       catch-up if the machine clock was changed or a test clock was used. */
    sqlite3_stmt *st = NULL;
    sqlite3_int64 last = 0;
    if (sqlite3_prepare_v2(reality_db, "SELECT last_tick FROM r2_reality_self WHERE id=1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) last = sqlite3_column_int64(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    if (last > 0 && now > last && now - last <= 7 * 86400) {
        update_hunger_locked((double)(now - last));
        st = NULL;
        if (sqlite3_prepare_v2(reality_db, "INSERT INTO r2_reality_ticks(previous_tick,current_tick,elapsed_seconds) VALUES(?,?,?)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, last); sqlite3_bind_int64(st, 2, now); sqlite3_bind_int64(st, 3, now-last); sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
    }
    st = NULL;
    if (sqlite3_prepare_v2(reality_db, "UPDATE r2_reality_self SET last_tick=?, updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now); sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    reality_ready = 1;
    pthread_mutex_unlock(&reality_lock);
    sync_room_mirrors();

    bridge_event("reality_engine_started", "R2's persistent reality engine started.",
        "Self-continuity and world-continuity are stored in dedicated r2_reality.db; Life Log and searchable memory remain in r2_memory.db. Room folders include room/, shelf/, box/, pockets/, wallet/, and toy_box/.", 1, 0);
    return 0;
}

void r2_reality_shutdown(void)
{
    pthread_mutex_lock(&reality_lock);
    if (reality_db) {
        sqlite3_wal_checkpoint_v2(reality_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(reality_db);
        reality_db = NULL;
    }
    reality_ready = 0;
    pthread_mutex_unlock(&reality_lock);
}

int r2_reality_is_initialized(void)
{
    pthread_mutex_lock(&reality_lock);
    int ready = reality_ready;
    pthread_mutex_unlock(&reality_lock);
    return ready;
}


static void maybe_record_starvation(void)
{
    if (!r2_reality_is_initialized()) return;
    double since_meal = 0.0;
    char logged[32] = {0};
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT seconds_since_meal FROM r2_reality_self WHERE id=1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        since_meal = sqlite3_column_double(st, 0);
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT value FROM r2_reality_self_facts WHERE key='starvation_72h_logged'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) snprintf(logged, sizeof(logged), "%s", (const char *)v);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);

    if (since_meal >= 72.0 * 3600.0 && strcmp(logged, "yes") != 0) {
        if (r2_reality_set_self("starvation_72h_logged", "yes",
                "The modeled elapsed time since the last qualifying meal reached 72 hours." ) == 0) {
            bridge_event("prolonged_starvation",
                "R2 reached 72 modeled hours without a qualifying meal.",
                "Hunger remains at its maximum modeled level. This is a simulation state, not a claim of biological metabolism.",
                1, 1);
        }
    }
}

int r2_reality_tick(void)
{
    if (!r2_reality_is_initialized()) return -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL), last = 0;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT last_tick FROM r2_reality_self WHERE id=1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) last = sqlite3_column_int64(st, 0);
    if (st) sqlite3_finalize(st);
    if (last > 0 && now > last && now - last <= 7 * 86400) {
        update_hunger_locked((double)(now-last));
        st = NULL;
        if (sqlite3_prepare_v2(reality_db, "INSERT INTO r2_reality_ticks(previous_tick,current_tick,elapsed_seconds) VALUES(?,?,?)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, last); sqlite3_bind_int64(st, 2, now); sqlite3_bind_int64(st, 3, now-last); sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
    }
    /* Collection memory loses precision with age; the physical object table
       is never changed by this memory-decay operation. */
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_item_memory SET precision='vague', exact_quantity=NULL, approximate_quantity=NULL, last_decay_at=? WHERE precision!='vague' AND collected_at<=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, now - 30 * 86400);
        sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_item_memory SET precision='approximate', approximate_quantity=MAX(1,CAST((exact_quantity+2)/5 AS INTEGER)*5), exact_quantity=NULL, last_decay_at=? WHERE precision='exact' AND collected_at<=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, now - 7 * 86400);
        sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    int ok = sqlite3_prepare_v2(reality_db, "UPDATE r2_reality_self SET last_tick=?, updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL) == SQLITE_OK;
    if (ok) { sqlite3_bind_int64(st, 1, now); ok = sqlite3_step(st) == SQLITE_DONE; }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (ok) maybe_record_starvation();
    return ok ? 0 : -1;
}

static char *query_text(const char *sql, const char *arg)
{
    if (!r2_reality_is_initialized()) return NULL;
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK && arg) bind_text(st, 1, arg);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *a = sqlite3_column_text(st, 0);
        const unsigned char *b = sqlite3_column_text(st, 1);
        const unsigned char *c = sqlite3_column_text(st, 2);
        char line[8192];
        snprintf(line, sizeof(line), "%s%s%s%s%s%s\n", a?(const char*)a:"", b?" — ":"", b?(const char*)b:"", c?" (":"", c?(const char*)c:"", c?")":"");
        size_t n = strlen(line);
        if (len+n+1 > cap) {
            size_t next=cap;
            while(next<len+n+1) next*=2;
            if(next>REALITY_MAX_OUTPUT) { break; }
            char *grown=realloc(out,next);
            if(!grown) break;
            out=grown; cap=next;
        }
        memcpy(out+len,line,n); len+=n; out[len]='\0';
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE && len == 0) { free(out); return NULL; }
    if (!len) snprintf(out, cap, "(nothing here yet)\n");
    return out;
}

char *r2_reality_list(const char *container)
{
    const char *c = container && *container ? container : "room";
    char sql[] = "SELECT name,description,printf('x%d in %s',quantity,container) FROM r2_reality_objects WHERE container=? ORDER BY name COLLATE NOCASE";
    return query_text(sql, c);
}

char *r2_reality_room_look(void)
{
    if (r2_reality_tick() != 0) return NULL;
    char *room = r2_reality_list("room");
    char *shelf = r2_reality_list("shelf");
    char *box = r2_reality_list("box");
    char *pockets = r2_reality_list("pockets");
    char *wallet = r2_reality_list("wallet");
    char *named = query_text("SELECT c.name,group_concat(o.name, ', '),printf('%d items',count(o.id)) FROM r2_reality_containers c LEFT JOIN r2_reality_objects o ON o.container=c.name WHERE c.parent='room' AND c.name NOT IN ('room','shelf','box') GROUP BY c.name ORDER BY c.name",NULL);
    if (!room || !shelf || !box || !pockets || !wallet || !named) {
        free(room); free(shelf); free(box); free(pockets); free(wallet); free(named); return NULL;
    }
    size_t n = strlen(room)+strlen(shelf)+strlen(box)+strlen(pockets)+strlen(wallet)+strlen(named)+640;
    char *out = malloc(n);
    if (out) snprintf(out,n,
        "R2'S ROOM\nRoom: %sShelf: %sBox: %sNamed containers and their contents:\n%sPockets: %sWallet: %s",
        room,shelf,box,named,pockets,wallet);
    free(room); free(shelf); free(box); free(pockets); free(wallet); free(named);
    return out;
}

char *r2_reality_status(void)
{
    if (r2_reality_tick() != 0) return NULL;
    if (!r2_reality_is_initialized()) return NULL;
    char *out = malloc(4096);
    if (!out) return NULL;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;
    double hunger=0, since=0, sleepiness=0, energy=100;
    if (sqlite3_prepare_v2(reality_db,"SELECT hunger,seconds_since_meal,sleepiness,energy FROM r2_reality_self WHERE id=1",-1,&st,NULL)==SQLITE_OK &&
        sqlite3_step(st)==SQLITE_ROW) {
        hunger=sqlite3_column_double(st,0); since=sqlite3_column_double(st,1);
        sleepiness=sqlite3_column_double(st,2); energy=sqlite3_column_double(st,3);
    }
    if(st) sqlite3_finalize(st);
    int count=0;
    if(sqlite3_prepare_v2(reality_db,"SELECT count(*) FROM r2_reality_objects",-1,&st,NULL)==SQLITE_OK &&
       sqlite3_step(st)==SQLITE_ROW) count=sqlite3_column_int(st,0);
    if(st) sqlite3_finalize(st);
    st = NULL;
    sqlite3_int64 world_elapsed = 0;
    if (sqlite3_prepare_v2(reality_db,"SELECT value FROM r2_reality_meta WHERE key='world_elapsed_seconds'",-1,&st,NULL)==SQLITE_OK &&
        sqlite3_step(st)==SQLITE_ROW) {
        const unsigned char *v=sqlite3_column_text(st,0);
        if(v) world_elapsed=(sqlite3_int64)atoll((const char*)v);
    }
    if(st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    const char *hstate = hunger < 25 ? "satisfied" : hunger < 50 ? "getting hungry" : hunger < 75 ? "hungry" : hunger < 100 ? "very hungry" : "starving";
    snprintf(out,4096,"SELF CONTINUITY\nHunger: %.1f/100 (%s)\nTime since meal: %.1f hours\nSleepiness: %.1f/100\nEnergy: %.1f/100\nModeled world time elapsed: %lld days, %lld hours\nObjects tracked in the world: %d\nAfter 72 hours without food, prolonged starvation is recorded; needs do not magically reset on restart.\n",
        hunger,hstate,since/3600.0,sleepiness,energy,
        (long long)(world_elapsed/86400),(long long)((world_elapsed%86400)/3600),count);
    return out;
}

char *r2_reality_context(void)
{
    char *status = r2_reality_status();
    char *room = r2_reality_room_look();
    char *facts = query_text("SELECT key,value,evidence FROM r2_reality_self_facts ORDER BY updated_at DESC", NULL);
    char *items_memory = query_text(
        "SELECT item_name,CASE precision WHEN 'exact' THEN printf('remembers collecting exactly %d',exact_quantity) WHEN 'approximate' THEN printf('remembers collecting about %d',approximate_quantity) ELSE 'remembers collecting some; exact quantity and timing have faded' END,CASE precision WHEN 'exact' THEN printf('%d days ago',MAX(0,(strftime('%s','now')-collected_at)/86400)) WHEN 'approximate' THEN 'older collection memory; approximate quantity' ELSE 'older vague memory' END FROM r2_reality_item_memory ORDER BY collected_at DESC LIMIT 80", NULL);
    char *food_preferences = query_text("SELECT ingredient,printf('average satisfaction %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_ingredient_preferences WHERE rating_count>0 ORDER BY satisfaction_sum*1.0/rating_count DESC", NULL);
    char *food_experiences = query_text("SELECT food_name,printf('satisfaction %+d/2',satisfaction),notes FROM r2_food_experiences WHERE satisfaction IS NOT NULL ORDER BY eaten_at DESC LIMIT 20", NULL);
    char *foods = food_metrics_context();
    if (!status || !room || !facts || !items_memory || !food_preferences || !food_experiences || !foods) {
        free(status); free(room); free(facts); free(items_memory); free(food_preferences); free(food_experiences); free(foods);
        return NULL;
    }
    size_t n = strlen(status) + strlen(room) + strlen(facts) + strlen(items_memory) + strlen(food_preferences) + strlen(food_experiences) + strlen(foods) + 2600;
    char *out = malloc(n);
    if (out) snprintf(out, n,
        "PERSISTENT REALITY CONTEXT (authoritative database state; do not invent changes):\n"
        "%s\n%s\nSELF-CONTINUITY FACTS:\n%s\nCOLLECTION MEMORIES (precision intentionally fades; not current inventory):\n%s\nLEARNED FOOD/INGREDIENT PREFERENCES (subjective scores):\n%s\nRECENT RATED FOOD EXPERIENCES:\n%s\nAVAILABLE FOOD METRICS:\n%s\n"
        "WORLD ACTIONS: Put one action on its own line. Use [WORLD] look to inspect the room; "
        "[WORLD] add|name|description|container|quantity to create an item; "
        "[WORLD] move|name|container to move it; [WORLD] remove|name to remove it; "
        "[WORLD] eat|food|fullness_points (or auto for XML) to update hunger; "
        "[WORLD] sleep|hours to advance sleep recovery and trigger a private dream simulation; "
        "[WORLD] dream|description to record a reported dream; "
        "[WORLD] ratefood|food|-2..2|reason to rate the most recent unrated eating experience. "
        "[WORLD] self|key|value|evidence to record a self-state fact. "
        "Food metrics live in room/food_metrics.xml; each food can define fullness, energy, ingredients (comma-separated), and taste (sensory description). Use only listed metrics and auto rather than guessing. "
        "Containers: room, shelf, box, toy box, pockets, wallet; named containers can be created by moving an item to a new container name. "
        "Food fullness points are modeled values, not measured biological facts. Sleep advances hunger and world time. "
        "Dreams are stored as simulated reports, not waking facts. Ask before moving or deleting a user's important item. "
        "Do not claim an action succeeded unless the action result confirms it.",
        status, room, facts, items_memory, food_preferences, food_experiences, foods);
    free(status); free(room); free(facts); free(items_memory); free(food_preferences); free(food_experiences); free(foods);
    return out;
}

int r2_reality_add_item(const char *name,const char *description,const char *container,int quantity)
{
    if(!name || !*name || strlen(name)>REALITY_MAX_TEXT || (description && strlen(description)>REALITY_MAX_TEXT)) return -1;
    if(!container || !*container) container="room";
    if(quantity<1) quantity=1;
    if(!r2_reality_is_initialized()) return -1;
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *prior = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT container FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &prior, NULL) == SQLITE_OK) {
        bind_text(prior, 1, name);
        if (sqlite3_step(prior) == SQLITE_ROW) {
            const unsigned char *oc = sqlite3_column_text(prior, 0);
            if (oc) snprintf(old_container, sizeof(old_container), "%s", (const char *)oc);
        }
    }
    if (prior) sqlite3_finalize(prior);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"self":"room");rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    st=NULL;
    if(rc==SQLITE_DONE) rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_objects(name,description,quantity,container,owner) VALUES(?,?,?,?, 'R2') ON CONFLICT(name) DO UPDATE SET description=excluded.description,quantity=r2_reality_objects.quantity+excluded.quantity,container=excluded.container,updated_at=CURRENT_TIMESTAMP",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,name);bind_text(st,2,description);sqlite3_bind_int(st,3,quantity);bind_text(st,4,container);rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        st = NULL;
        if (sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_item_memory(item_name,description,exact_quantity,precision,collected_at,last_decay_at) VALUES(?,?,?,'exact',?,?)",
            -1, &st, NULL) == SQLITE_OK) {
            sqlite3_int64 collected = (sqlite3_int64)time(NULL);
            bind_text(st, 1, name);
            bind_text(st, 2, description);
            sqlite3_bind_int(st, 3, quantity);
            sqlite3_bind_int64(st, 4, collected);
            sqlite3_bind_int64(st, 5, collected);
            if (sqlite3_step(st) != SQLITE_DONE) rc = SQLITE_ERROR;
        } else rc = SQLITE_ERROR;
        if (st) sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE) return -1;
    if (*old_container) mirror_remove(name, old_container);
    if (mirror_write(name, description, quantity, container) != 0)
        fprintf(stderr, "[R2 Reality] Database saved, but room mirror file could not be updated for '%s'.\n", name);
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 recorded item '%s' in %s.",name,container);
    snprintf(details,sizeof(details),"Item=%s; description=%s; container=%s; quantity=%d",name,description?description:"",container,quantity);
    bridge_event("object_added",summary,details,1,1);
    return 0;
}

int r2_reality_move_item(const char *name,const char *container)
{
    if(!name||!*name||!container||!*container||strlen(name)>REALITY_MAX_TEXT||strlen(container)>REALITY_MAX_TEXT||!r2_reality_is_initialized()) return -1;
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    char old_description[REALITY_MAX_TEXT + 1] = {0};
    int old_quantity = 1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *prior = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT container,description,quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &prior, NULL) == SQLITE_OK) {
        bind_text(prior, 1, name);
        if (sqlite3_step(prior) == SQLITE_ROW) {
            const unsigned char *oc = sqlite3_column_text(prior, 0);
            const unsigned char *od = sqlite3_column_text(prior, 1);
            if (oc) snprintf(old_container, sizeof(old_container), "%s", (const char *)oc);
            if (od) snprintf(old_description, sizeof(old_description), "%s", (const char *)od);
            old_quantity = sqlite3_column_int(prior, 2);
        }
    }
    if (prior) sqlite3_finalize(prior);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"self":"room");rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    st=NULL;
    if(rc==SQLITE_DONE) rc=sqlite3_prepare_v2(reality_db,"UPDATE r2_reality_objects SET container=?,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,name);rc=sqlite3_step(st);if(rc==SQLITE_DONE && sqlite3_changes(reality_db)==0)rc=SQLITE_NOTFOUND;}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE) return -1;
    if (*old_container) mirror_remove(name, old_container);
    if (mirror_write(name, old_description, old_quantity, container) != 0)
        fprintf(stderr, "[R2 Reality] Database saved, but room mirror file could not be updated for '%s'.\n", name);
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 moved '%s' to %s.",name,container);
    snprintf(details,sizeof(details),"Object=%s; destination=%s",name,container);
    bridge_event("object_moved",summary,details,1,1);
    return 0;
}

int r2_reality_remove_item(const char *name)
{
    if(!name||!*name||!r2_reality_is_initialized())return -1;
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *prior = NULL;
    if (sqlite3_prepare_v2(reality_db, "SELECT container FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &prior, NULL) == SQLITE_OK) {
        bind_text(prior, 1, name);
        if (sqlite3_step(prior) == SQLITE_ROW) {
            const unsigned char *oc = sqlite3_column_text(prior, 0);
            if (oc) snprintf(old_container, sizeof(old_container), "%s", (const char *)oc);
        }
    }
    if (prior) sqlite3_finalize(prior);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,name);rc=sqlite3_step(st);if(rc==SQLITE_DONE&&sqlite3_changes(reality_db)==0)rc=SQLITE_NOTFOUND;}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE)return -1;
    if (*old_container) mirror_remove(name, old_container);
    char summary[512];snprintf(summary,sizeof(summary),"R2 removed '%s' from his tracked world.",name);
    bridge_event("object_removed",summary,"The object was removed from R2's persistent object inventory.",1,1);
    return 0;
}


int r2_reality_eat(const char *food, double fullness)
{
    if (!food || !*food || !r2_reality_is_initialized()) return -1;
    double energy_bonus = 0.0;
    if (fullness >= 0.0) {
        if (fullness > 100.0) fullness = 100.0;
        energy_bonus = fullness * 0.1;
    }
    if (fullness < 0.0) {
        if (food_metric(food, &fullness, &energy_bonus) != 0) {
            fprintf(stderr, "[R2 Reality] No food metric found for '%s' in room/food_metrics.xml.\n", food);
            return -1;
        }
    }
    if (fullness > 100.0) fullness = 100.0;
    if (energy_bonus < 0.0) energy_bonus = 0.0;
    if (energy_bonus > 100.0) energy_bonus = 100.0;
    if (r2_reality_tick() != 0) return -1;
    char ingredients[1024] = {0}, taste[1024] = {0};
    food_attributes(food, ingredients, sizeof(ingredients), taste, sizeof(taste));

    char container[REALITY_MAX_TEXT + 1] = {0};
    char description[REALITY_MAX_TEXT + 1] = {0};
    int quantity = 0;
    int consumed_tracked_item = 0;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT container,description,quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, food);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *co = sqlite3_column_text(st, 0);
            const unsigned char *de = sqlite3_column_text(st, 1);
            if (co) snprintf(container, sizeof(container), "%s", (const char *)co);
            if (de) snprintf(description, sizeof(description), "%s", (const char *)de);
            quantity = sqlite3_column_int(st, 2);
        }
    }
    if (st) sqlite3_finalize(st);
    st = NULL;

    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=MAX(0,hunger-?), seconds_since_meal=CASE WHEN ?>=10 THEN 0 ELSE seconds_since_meal END, energy=MIN(100,energy+?), updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_double(st, 1, fullness);
        sqlite3_bind_double(st, 2, fullness);
        sqlite3_bind_double(st, 3, energy_bonus);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;

    /* If the food is a tracked world object, consuming it changes the world
       too: one unit disappears, or the remaining quantity is decremented. */
    if (rc == SQLITE_DONE && quantity > 0) {
        if (quantity > 1) {
            if (sqlite3_prepare_v2(reality_db,
                "UPDATE r2_reality_objects SET quantity=quantity-1,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE",
                -1, &st, NULL) == SQLITE_OK) {
                bind_text(st, 1, food);
                rc = sqlite3_step(st);
                consumed_tracked_item = (rc == SQLITE_DONE);
            } else rc = SQLITE_ERROR;
        } else {
            if (sqlite3_prepare_v2(reality_db,
                "DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
                -1, &st, NULL) == SQLITE_OK) {
                bind_text(st, 1, food);
                rc = sqlite3_step(st);
                consumed_tracked_item = (rc == SQLITE_DONE);
            } else rc = SQLITE_ERROR;
        }
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE && fullness >= 10.0) {
        st = NULL;
        if (sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_self_facts(key,value,evidence) VALUES('starvation_72h_logged','no','Reset by qualifying food intake') ON CONFLICT(key) DO UPDATE SET value='no',evidence='Reset by qualifying food intake',updated_at=CURRENT_TIMESTAMP",
            -1, &st, NULL) == SQLITE_OK)
            sqlite3_step(st);
        if (st) sqlite3_finalize(st);
    }
    if (rc == SQLITE_DONE) {
        st = NULL;
        if (sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_food_experiences(food_name,ingredients,fullness,energy_bonus,eaten_at) VALUES(?,?,?,?,?)",
            -1, &st, NULL) == SQLITE_OK) {
            bind_text(st, 1, food);
            bind_text(st, 2, ingredients);
            sqlite3_bind_double(st, 3, fullness);
            sqlite3_bind_double(st, 4, energy_bonus);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
            if (sqlite3_step(st) != SQLITE_DONE) rc = SQLITE_ERROR;
        } else rc = SQLITE_ERROR;
        if (st) sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;

    if (consumed_tracked_item) {
        if (quantity == 1) mirror_remove(food, container);
        else if (mirror_write(food, description, quantity - 1, container) != 0)
            fprintf(stderr, "[R2 Reality] Food state saved, but mirror could not be updated for '%s'.\n", food);
    }

    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 ate %s; his hunger need was reduced by %.1f points.", food, fullness);
    snprintf(details, sizeof(details), "Food=%s; modeled fullness contribution=%.1f/100; tracked object consumed=%s; this is a modeled value, not a measured biological quantity.",
             food, fullness, consumed_tracked_item ? "yes" : "no");
    bridge_event("food_consumed", summary, details, 1, 1);
    return 0;
}


char *r2_reality_food_context(const char *food)
{
    if (!food || !*food || !r2_reality_is_initialized()) return NULL;
    char ingredients[1024] = {0}, taste[1024] = {0};
    double fullness = -1.0, energy = 0.0;
    (void)food_metric(food, &fullness, &energy);
    food_attributes(food, ingredients, sizeof(ingredients), taste, sizeof(taste));
    char *preferences = query_text(
        "SELECT ingredient,printf('average satisfaction %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_ingredient_preferences WHERE rating_count>0 ORDER BY satisfaction_sum*1.0/rating_count DESC", NULL);
    char *history = query_text(
        "SELECT food_name,printf('satisfaction %+d/2',satisfaction),notes FROM r2_food_experiences WHERE food_name=? COLLATE NOCASE AND satisfaction IS NOT NULL ORDER BY eaten_at DESC LIMIT 10", food);
    char *food_pref = query_text(
        "SELECT food_name,printf('average satisfaction %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_preferences WHERE food_name=? COLLATE NOCASE AND rating_count>0", food);
    if (!preferences || !history || !food_pref) {
        free(preferences); free(history); free(food_pref); return NULL;
    }
    size_t n = strlen(food)+strlen(ingredients)+strlen(taste)+strlen(preferences)+strlen(history)+strlen(food_pref)+1024;
    char *out = malloc(n);
    if (out) snprintf(out,n,
        "FOOD EXPERIENCE CONTEXT (learned subjective preferences; not hard-coded):\n"
        "Food: %s\nConfigured fullness: %s%.1f/100\nConfigured energy bonus: %.1f\nIngredients: %s\nSensory description: %s\n"
        "Past experiences with this food:\n%sFood preference summary:\n%sIngredient preference summaries:\n%s"
        "Satisfaction is a subjective modeled rating from -2 (strong dislike) to +2 (strong enjoyment); 0 means neutral/uncertain. "
        "Fullness and satisfaction are independent. Ingredient summaries are learned only from rated eating experiences.",
        food, fullness < 0 ? "not configured; " : "", fullness < 0 ? 0.0 : fullness, energy,
        *ingredients ? ingredients : "not specified", *taste ? taste : "not specified",
        history, food_pref, preferences);
    free(preferences); free(history); free(food_pref);
    return out;
}

int r2_reality_rate_food(const char *food, int satisfaction, const char *notes)
{
    if (!food || !*food || satisfaction < -2 || satisfaction > 2 ||
        !r2_reality_is_initialized()) return -1;
    char ingredients[1024] = {0}, taste[1024] = {0};
    food_attributes(food, ingredients, sizeof(ingredients), taste, sizeof(taste));
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    sqlite3_int64 experience_id = 0;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT id FROM r2_food_experiences WHERE food_name=? COLLATE NOCASE AND satisfaction IS NULL ORDER BY eaten_at DESC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, food);
        if (sqlite3_step(st) == SQLITE_ROW) experience_id = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    int rc = SQLITE_ERROR;
    if (experience_id && sqlite3_prepare_v2(reality_db,
        "UPDATE r2_food_experiences SET satisfaction=?,notes=?,rated_at=? WHERE id=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, satisfaction);
        bind_text(st, 2, notes ? notes : "");
        sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
        sqlite3_bind_int64(st, 4, experience_id);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (rc == SQLITE_DONE && sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_food_preferences(food_name,satisfaction_sum,rating_count,updated_at) VALUES(?,?,1,?) ON CONFLICT(food_name) DO UPDATE SET satisfaction_sum=satisfaction_sum+excluded.satisfaction_sum,rating_count=rating_count+1,updated_at=excluded.updated_at",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, food);
        sqlite3_bind_double(st, 2, (double)satisfaction);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
        rc = sqlite3_step(st);
    } else if (rc == SQLITE_DONE) rc = SQLITE_ERROR;
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;

    char *copy = strdup(ingredients);
    if (copy) {
        char *save = NULL;
        for (char *part = strtok_r(copy, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
            while (isspace((unsigned char)*part)) part++;
            size_t len = strlen(part);
            while (len && isspace((unsigned char)part[len-1])) part[--len] = '\0';
            if (!*part) continue;
            pthread_mutex_lock(&reality_lock);
            st = NULL;
            if (sqlite3_prepare_v2(reality_db,
                "INSERT INTO r2_food_ingredient_preferences(ingredient,satisfaction_sum,rating_count,updated_at) VALUES(?,?,1,?) ON CONFLICT(ingredient) DO UPDATE SET satisfaction_sum=satisfaction_sum+excluded.satisfaction_sum,rating_count=rating_count+1,updated_at=excluded.updated_at",
                -1, &st, NULL) == SQLITE_OK) {
                bind_text(st, 1, part);
                sqlite3_bind_double(st, 2, (double)satisfaction);
                sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
                sqlite3_step(st);
            }
            if (st) sqlite3_finalize(st);
            pthread_mutex_unlock(&reality_lock);
        }
        free(copy);
    }
    char summary[512], details[2048];
    snprintf(summary,sizeof(summary),"R2's modeled satisfaction with %s was rated %+d/2.",food,satisfaction);
    snprintf(details,sizeof(details),"Food=%s; ingredients=%s; sensory description=%s; satisfaction=%d/2; reason=%s. This is a learned subjective simulation, not an externally verified reaction.",
        food,*ingredients?ingredients:"not specified",*taste?taste:"not specified",satisfaction,notes?notes:"not supplied");
    bridge_event("food_preference_learned",summary,details,1,0);
    return 0;
}

int r2_reality_sleep(double hours)
{
    if (!r2_reality_is_initialized() || hours <= 0.0 || hours > 48.0) return -1;
    if (r2_reality_tick() != 0) return -1;
    pthread_mutex_lock(&reality_lock);
    /* Sleeping advances the modeled body clock: needs still accrue during
       sleep, then rest restores energy and reduces sleepiness. */
    update_hunger_locked(hours * 3600.0);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET sleepiness=MAX(0,sleepiness-?), energy=MIN(100,energy+?), updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hours * 12.5);
        sqlite3_bind_double(st, 2, hours * 10.0);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 slept for %.2f modeled hours.", hours);
    snprintf(details, sizeof(details), "Sleep duration=%.2f hours; sleepiness reduced by %.2f points and energy restored by %.2f points, capped at 100.", hours, hours*12.5, hours*10.0);
    bridge_event("sleep_completed", summary, details, 1, 1);
    return 0;
}

int r2_reality_record_dream(const char *description)
{
    if (!description || !*description || strlen(description) > REALITY_MAX_TEXT) return -1;
    int rc = r2_reality_set_self("last_reported_dream", description,
        "A dream description explicitly reported or authored by R2; not independently verified.");
    if (rc == 0) {
        bridge_event("dream_reported", "R2 recorded a private simulated dream.",
                     description, 0, 0);
        size_t n = strlen(description) + 128;
        char *entry = malloc(n);
        if (entry) {
            snprintf(entry, n, "Private simulated dream report (not a waking event):\n%s", description);
            if (r2_diary_active()) r2_diary_write(entry);
            free(entry);
        }
    }
    return rc;
}

int r2_reality_set_self(const char *key,const char *value,const char *evidence)
{
    if(!key||!*key||!value||strlen(key)>256||strlen(value)>REALITY_MAX_TEXT||!r2_reality_is_initialized())return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_self_facts(key,value,evidence) VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,evidence=excluded.evidence,updated_at=CURRENT_TIMESTAMP",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,key);bind_text(st,2,value);bind_text(st,3,evidence);rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE)return -1;
    char summary[512],details[2048];snprintf(summary,sizeof(summary),"R2 self-continuity updated: %s = %s.",key,value);
    snprintf(details,sizeof(details),"State key=%s; value=%s; evidence=%s",key,value,evidence?evidence:"not supplied");
    bridge_event("self_state_updated",summary,details,1,0);
    return 0;
}

char *r2_reality_get_self(const char *key)
{
    if(!key||!r2_reality_is_initialized())return NULL;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;char *out=NULL;
    if(sqlite3_prepare_v2(reality_db,"SELECT value FROM r2_reality_self_facts WHERE key=?",-1,&st,NULL)==SQLITE_OK){
        bind_text(st,1,key);
        if(sqlite3_step(st)==SQLITE_ROW){const unsigned char *v=sqlite3_column_text(st,0);if(v)out=strdup((const char*)v);}
    }
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    return out;
}
