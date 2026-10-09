#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "Reality.h"
#include "r2_diary.h"
#include "Log.h"
#include "r2.h"

#include <ctype.h>
#include <errno.h>
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
    char room[1024], shelf[1100], box[1100];
    snprintf(room, sizeof(room), "%s/room", R2_ROOT);
    snprintf(shelf, sizeof(shelf), "%s/shelf", room);
    snprintf(box, sizeof(box), "%s/box", room);
    if (mkdir_one(room) || mkdir_one(shelf) || mkdir_one(box)) {
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
    else return 1; /* Inventory and named containers are database-only. */
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
}

int r2_reality_init(void)
{
    pthread_mutex_lock(&reality_lock);
    if (reality_ready) { pthread_mutex_unlock(&reality_lock); return 0; }
    if (make_room_dirs() != 0) { pthread_mutex_unlock(&reality_lock); return -1; }

    int rc = sqlite3_open_v2(R2_DIARY_DATABASE, &reality_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Reality] Cannot open shared database %s: %s\n",
                R2_DIARY_DATABASE, reality_db ? sqlite3_errmsg(reality_db) : "unknown error");
        if (reality_db) sqlite3_close(reality_db);
        reality_db = NULL;
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    sqlite3_busy_timeout(reality_db, 5000);
    sqlite3_exec(reality_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_reality_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
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

    bridge_event("reality_engine_started", "R2's persistent reality engine started.",
        "Self-continuity and world-continuity are stored separately and advance together in r2_memory.db. Room directories: room/, room/shelf/, room/box/.", 1, 0);
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
    st = NULL;
    int ok = sqlite3_prepare_v2(reality_db, "UPDATE r2_reality_self SET last_tick=?, updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL) == SQLITE_OK;
    if (ok) { sqlite3_bind_int64(st, 1, now); ok = sqlite3_step(st) == SQLITE_DONE; }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
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
    pthread_mutex_unlock(&reality_lock);
    const char *hstate = hunger < 25 ? "satisfied" : hunger < 50 ? "getting hungry" : hunger < 75 ? "hungry" : hunger < 100 ? "very hungry" : "starving";
    snprintf(out,4096,"SELF CONTINUITY\nHunger: %.1f/100 (%s)\nTime since meal: %.1f hours\nSleepiness: %.1f/100\nEnergy: %.1f/100\nObjects tracked in the world: %d\nAfter 72 hours without food, prolonged starvation is recorded; needs do not magically reset on restart.\n",
        hunger,hstate,since/3600.0,sleepiness,energy,count);
    return out;
}

char *r2_reality_context(void)
{
    char *status=r2_reality_status();
    char *room=r2_reality_room_look();
    char *facts=query_text("SELECT key,value,evidence FROM r2_reality_self_facts ORDER BY updated_at DESC",NULL);
    if(!status || !room || !facts) { free(status); free(room); free(facts); return NULL; }
    size_t n=strlen(status)+strlen(room)+strlen(facts)+1400;
    char *out=malloc(n);
    if(out) snprintf(out,n,
        "PERSISTENT REALITY CONTEXT (authoritative database state; do not invent changes):\n%s\n%s\nSELF-CONTINUITY FACTS:\n%s\n"
        "WORLD ACTIONS: Use [WORLD] look to inspect the room; [WORLD] add|name|description|container|quantity to create an item; [WORLD] move|name|container to move it; [WORLD] remove|name to remove it; [WORLD] eat|food|fullness_points (or auto for XML) to update hunger; [WORLD] sleep|hours to advance sleep recovery; [WORLD] dream|description to record a reported dream; [WORLD] self|key|value|evidence to record a self-state fact. Food metrics live in room/food_metrics.xml; use auto rather than guessing when a food entry exists. Containers: room, shelf, box, pockets, wallet; named containers can be created by moving an item to a new container name. Food fullness points are modeled values, not measured biological facts. Sleep still advances hunger and elapsed need state. Dreams are stored as reports, not independently verified facts. Ask before moving or deleting a user's important item. Do not claim an action succeeded unless the action result confirms it.",
        status,room,facts);
    free(status); free(room); free(facts);
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
    if(rc==SQLITE_DONE) rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_objects(name,description,quantity,container,owner) VALUES(?,?,?,?, 'R2') ON CONFLICT(name) DO UPDATE SET description=excluded.description,quantity=excluded.quantity,container=excluded.container,updated_at=CURRENT_TIMESTAMP",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,name);bind_text(st,2,description);sqlite3_bind_int(st,3,quantity);bind_text(st,4,container);rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
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
    if (rc == 0)
        bridge_event("dream_reported", "R2 recorded a dream report.",
                     description, 0, 1);
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
