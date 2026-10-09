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

static void update_hunger_locked(double elapsed)
{
    sqlite3_stmt *st = NULL;
    double hunger = 0.0;
    double since_meal = 0.0;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT hunger, seconds_since_meal FROM r2_reality_self WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        hunger = sqlite3_column_double(st, 0);
        since_meal = sqlite3_column_double(st, 1);
    }
    if (st) sqlite3_finalize(st);
    hunger += elapsed * HUNGER_PER_SECOND;
    if (hunger > 100.0) hunger = 100.0;
    since_meal += elapsed;
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=?, seconds_since_meal=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hunger);
        sqlite3_bind_double(st, 2, since_meal);
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
        "('room','room','R2\'s room',''),('shelf','surface','The shelf in R2\'s room','room'),('box','container','The general storage box in R2\'s room','room'),('pockets','inventory','R2\'s pockets','self'),('wallet','inventory','R2\'s wallet','self');";
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
    while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *a = sqlite3_column_text(st, 0);
        const unsigned char *b = sqlite3_column_text(st, 1);
        const unsigned char *c = sqlite3_column_text(st, 2);
        char line[8192];
        snprintf(line, sizeof(line), "%s%s%s%s%s\n", a?(const char*)a:"", b?" — ":"", b?(const char*)b:"", c?" (":"", c?(const char*)c:"");
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
    if (!room || !shelf || !box || !pockets || !wallet) {
        free(room); free(shelf); free(box); free(pockets); free(wallet); return NULL;
    }
    size_t n = strlen(room)+strlen(shelf)+strlen(box)+strlen(pockets)+strlen(wallet)+512;
    char *out = malloc(n);
    if (out) snprintf(out,n,
        "R2'S ROOM\nRoom: %sShelf: %sBox: %sPockets: %sWallet: %s",
        room,shelf,box,pockets,wallet);
    free(room); free(shelf); free(box); free(pockets); free(wallet);
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
    if(!status || !room) { free(status); free(room); return NULL; }
    size_t n=strlen(status)+strlen(room)+512;
    char *out=malloc(n);
    if(out) snprintf(out,n,
        "PERSISTENT REALITY CONTEXT (authoritative database state; do not invent changes):\n%s\n%s\n"
        "WORLD ACTIONS: Use [WORLD] look to inspect the room; [WORLD] add|name|description|container|quantity to create an item; [WORLD] move|name|container to move it; [WORLD] remove|name to remove it; [WORLD] self|key|value|evidence to record a self-state fact. Containers: room, shelf, box, pockets, wallet; named containers can be created by moving an item to a new container name. Ask before moving or deleting a user's important item. Do not claim an action succeeded unless the action result confirms it.",
        status,room);
    free(status); free(room);
    return out;
}

int r2_reality_add_item(const char *name,const char *description,const char *container,int quantity)
{
    if(!name || !*name || strlen(name)>REALITY_MAX_TEXT || (description && strlen(description)>REALITY_MAX_TEXT)) return -1;
    if(!container || !*container) container="room";
    if(quantity<1) quantity=1;
    if(!r2_reality_is_initialized()) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,(!strcmp(container,"shelf")||!strcmp(container,"box"))?"room":"");rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    st=NULL;
    if(rc==SQLITE_DONE) rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_objects(name,description,quantity,container,owner) VALUES(?,?,?,?, 'R2') ON CONFLICT(name) DO UPDATE SET description=excluded.description,quantity=excluded.quantity,container=excluded.container,updated_at=CURRENT_TIMESTAMP",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,name);bind_text(st,2,description);sqlite3_bind_int(st,3,quantity);bind_text(st,4,container);rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE) return -1;
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 recorded item '%s' in %s.",name,container);
    snprintf(details,sizeof(details),"Item=%s; description=%s; container=%s; quantity=%d",name,description?description:"",container,quantity);
    bridge_event("object_added",summary,details,1,1);
    return 0;
}

int r2_reality_move_item(const char *name,const char *container)
{
    if(!name||!*name||!container||!*container||strlen(name)>REALITY_MAX_TEXT||strlen(container)>REALITY_MAX_TEXT||!r2_reality_is_initialized()) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,(!strcmp(container,"pockets")||!strcmp(container,"wallet"))?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,(!strcmp(container,"shelf")||!strcmp(container,"box"))?"room":"");rc=sqlite3_step(st);}
    if(st)sqlite3_finalize(st);
    st=NULL;
    if(rc==SQLITE_DONE) rc=sqlite3_prepare_v2(reality_db,"UPDATE r2_reality_objects SET container=?,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,name);rc=sqlite3_step(st);if(rc==SQLITE_DONE && sqlite3_changes(reality_db)==0)rc=SQLITE_NOTFOUND;}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE) return -1;
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 moved '%s' to %s.",name,container);
    snprintf(details,sizeof(details),"Object=%s; destination=%s",name,container);
    bridge_event("object_moved",summary,details,1,1);
    return 0;
}

int r2_reality_remove_item(const char *name)
{
    if(!name||!*name||!r2_reality_is_initialized())return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,name);rc=sqlite3_step(st);if(rc==SQLITE_DONE&&sqlite3_changes(reality_db)==0)rc=SQLITE_NOTFOUND;}
    if(st)sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE)return -1;
    char summary[512];snprintf(summary,sizeof(summary),"R2 removed '%s' from his tracked world.",name);
    bridge_event("object_removed",summary,"The object was removed from R2's persistent object inventory.",1,1);
    return 0;
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
