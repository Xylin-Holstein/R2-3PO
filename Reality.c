#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "Reality.h"
#include "Addiction.h"
#include "Reward.h"
#include "r2_diary.h"
#include "Log.h"
#include "Visual.h"
#include "r2.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
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
static sqlite3 *fridge_db = NULL;
static pthread_mutex_t reality_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fridge_lock = PTHREAD_MUTEX_INITIALIZER;
static int reality_ready = 0;
static int reality_eat_internal(const char *food, double fullness, int consume_tracked_item, double energy_override);

/* Elapsed world time advances continuously. At +0.1 hunger every 4.75
 * seconds, hunger reaches 100 from zero in about 79 minutes; the 72-hour
 * mark is still explicitly described as prolonged starvation. */
static const double HUNGER_PER_SECOND = 0.1 / 4.75; /* +0.1 hunger and -0.1 satiety per 4.75 seconds */

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

static int ensure_dir_tree(const char *path)
{
    if (!path || !*path) return -1;
    char copy[4096]; size_t n = strlen(path);
    if (n >= sizeof(copy)) return -1;
    memcpy(copy, path, n + 1);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = '\0';
    for (char *p = copy + (copy[0] == '/' ? 1 : 0); ; ++p) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p; *p = '\0';
        if (*copy && mkdir_one(copy) != 0) return -1;
        *p = saved; if (!saved) break;
    }
    return 0;
}
static void money_mirror_write(sqlite3_int64 cash, sqlite3_int64 bank)
{
    char dir[1200], path[1400], tmp[1500];
    snprintf(dir,sizeof(dir),"%s/room/piggybank",R2_ROOT);
    if(ensure_dir_tree(dir)!=0)return;
    snprintf(path,sizeof(path),"%s/account.txt",dir); snprintf(tmp,sizeof(tmp),"%s.tmp.%ld",path,(long)getpid());
    FILE *fp=fopen(tmp,"w"); if(!fp)return;
    int bad=fprintf(fp,"cash=%.2f\nbank=%.2f\ntotal=%.2f\n",cash/100.0,bank/100.0,(cash+bank)/100.0)<0;
    if (fclose(fp) != 0) bad = 1;
    if (!bad && rename(tmp, path) != 0) bad = 1;
    if (bad) (void)unlink(tmp);
    char wallet[1200], cash_path[1400], cash_tmp[1500];
    snprintf(wallet, sizeof(wallet), "%s/pockets/wallet", R2_ROOT);
    if (ensure_dir_tree(wallet) != 0) return;
    snprintf(cash_path, sizeof(cash_path), "%s/cash.txt", wallet);
    snprintf(cash_tmp, sizeof(cash_tmp), "%s.tmp.%ld", cash_path, (long)getpid());
    fp = fopen(cash_tmp, "w");
    if (!fp) return;
    bad = fprintf(fp, "carried_cash=$%.2f\nThis file mirrors the persistent money account; it is not additional money.\n", cash / 100.0) < 0;
    if (fclose(fp) != 0) bad = 1;
    if (!bad && rename(cash_tmp, cash_path) != 0) bad = 1;
    if (bad) (void)unlink(cash_tmp);
}
static int money_read_locked(sqlite3_int64 *cash, sqlite3_int64 *bank)
{
    sqlite3_stmt *st=NULL; int rc=sqlite3_prepare_v2(reality_db,"SELECT cash_cents,bank_cents FROM r2_money_account WHERE id=1",-1,&st,NULL);
    if(rc==SQLITE_OK&&sqlite3_step(st)==SQLITE_ROW){if(cash)*cash=sqlite3_column_int64(st,0);if(bank)*bank=sqlite3_column_int64(st,1);rc=SQLITE_OK;}else rc=SQLITE_ERROR;
    if (st) sqlite3_finalize(st);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Move only generated mirror files when upgrading the old room-local layout.
 * User-created files are left untouched; SQLite remains the source of truth. */
static int migrate_mirror_directory(const char *old_dir, const char *new_dir)
{
    DIR *dir = opendir(old_dir);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        if (n < 7 || strcmp(entry->d_name + n - 7, ".r2item") != 0) continue;
        char old_path[2048], new_path[2048];
        int a = snprintf(old_path, sizeof(old_path), "%s/%s", old_dir, entry->d_name);
        int b = snprintf(new_path, sizeof(new_path), "%s/%s", new_dir, entry->d_name);
        if (a <= 0 || (size_t)a >= sizeof(old_path) || b <= 0 || (size_t)b >= sizeof(new_path)) {
            result = -1; continue;
        }
        if (access(new_path, F_OK) == 0) {
            if (unlink(old_path) != 0 && errno != ENOENT) result = -1;
        } else if (rename(old_path, new_path) != 0) result = -1;
    }
    closedir(dir);
    (void)rmdir(old_dir); /* Never recursively delete user-created content. */
    return result;
}

static int make_room_dirs(void)
{
    char room[1024], shelf[1100], box[1100], pockets[1100], wallet[1200];
    char fridge[1100], piggybank[1100], diary[1100], tapes[1200];
    char old_pockets[1200], old_wallet[1200], old_fridge[1200], old_toy_box[1200];
    snprintf(room,sizeof(room),"%s/room",R2_ROOT); snprintf(shelf,sizeof(shelf),"%s/shelf",room);
    snprintf(box,sizeof(box),"%s/box",room); snprintf(pockets,sizeof(pockets),"%s/pockets",R2_ROOT);
    snprintf(wallet,sizeof(wallet),"%s/wallet",pockets);
    snprintf(fridge,sizeof(fridge),"%s/fridge",R2_ROOT); snprintf(piggybank,sizeof(piggybank),"%s/piggybank",room);
    snprintf(old_pockets,sizeof(old_pockets),"%s/room/pockets",R2_ROOT);
    snprintf(old_wallet,sizeof(old_wallet),"%s/room/wallet",R2_ROOT);
    snprintf(old_fridge,sizeof(old_fridge),"%s/room/fridge",R2_ROOT);
    snprintf(old_toy_box,sizeof(old_toy_box),"%s/room/toy_box",R2_ROOT);
    snprintf(diary,sizeof(diary),"%s",R2_DIARY_DIR);
    snprintf(tapes,sizeof(tapes),"%s/VCR_Tapes",R2_ROOT);
    if(ensure_dir_tree(R2_ROOT)||ensure_dir_tree(R2_HOME)||ensure_dir_tree(diary)||ensure_dir_tree(room)||
       ensure_dir_tree(shelf)||ensure_dir_tree(box)||ensure_dir_tree(pockets)||ensure_dir_tree(wallet)||
       ensure_dir_tree(fridge)||ensure_dir_tree(piggybank)||ensure_dir_tree(tapes)) {
        fprintf(stderr, "[R2 Reality] Could not create room/shelf/box directories under %s\n", R2_ROOT);
        return -1;
    }
    if (migrate_mirror_directory(old_pockets, pockets) != 0 ||
        migrate_mirror_directory(old_wallet, wallet) != 0 ||
        migrate_mirror_directory(old_fridge, fridge) != 0 ||
        migrate_mirror_directory(old_toy_box, box) != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate one or more old mirror directories.\n");
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
                  "  <food name=\"burger\" fullness=\"100\" energy=\"10\" ingredients=\"bread,beef,cheese\" taste=\"savory,warm,salty\" />\n"
                  "</foods>\n", fp);
            fclose(fp);
        }
    }
    return 0;
}

static void append_factual_log_file(const char *type, const char *summary,
                                   const char *details)
{
    char directory[1200], path[1400];
    int n = snprintf(directory, sizeof(directory), "%s/log", R2_ROOT);
    if (n <= 0 || (size_t)n >= sizeof(directory)) return;
    if (ensure_dir_tree(directory) != 0) {
        fprintf(stderr, "[R2 Reality] Could not create factual log directory %s\n", directory);
        return;
    }
    n = snprintf(path, sizeof(path), "%s/log.txt", directory);
    if (n <= 0 || (size_t)n >= sizeof(path)) return;

    FILE *fp = fopen(path, "a");
    if (!fp) {
        fprintf(stderr, "[R2 Reality] Could not append factual event to %s\n", path);
        return;
    }
    time_t now = time(NULL);
    struct tm local_now;
    char timestamp[40] = "time-unavailable";
    if (localtime_r(&now, &local_now))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %z", &local_now);

    /* One complete, timestamped factual event per block. */
    fprintf(fp, "[%s] [%s] %s\n", timestamp,
            type && *type ? type : "world_event",
            summary && *summary ? summary : "(no summary)");
    if (details && *details)
        fprintf(fp, "  Details: %s\n", details);
    fputc('\n', fp);
    if (fclose(fp) != 0)
        fprintf(stderr, "[R2 Reality] Error closing factual log %s\n", path);
}

static void bridge_event(const char *type, const char *summary, const char *details,
                         int remember, int diary)
{
    /* The plain-text /log/log.txt and the structured Life Log are factual
       chronology. The private diary is reserved for reflection, not routine
       world events such as eating. */
    append_factual_log_file(type, summary, details);
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
static int reality_home_state = 1;
static int reality_is_home(void)
{
    return __atomic_load_n(&reality_home_state, __ATOMIC_ACQUIRE);
}
static int container_is_portable(const char *container)
{
    return container && (!strcasecmp(container, "pockets") || !strcasecmp(container, "wallet"));
}
static int container_accessible(const char *container)
{
    return container_is_portable(container) || reality_is_home();
}


/* Human-inspectable mirrors follow the real hierarchy: room storage stays
 * under room/, pockets and fridge are siblings of room/, and wallet is inside
 * pockets/. SQLite remains canonical; mirror files are projections. */
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
    char base[1400];
    if (!strcasecmp(container, "room")) snprintf(base, sizeof(base), "%s/room", R2_ROOT);
    else if (!strcasecmp(container, "shelf")) snprintf(base, sizeof(base), "%s/room/shelf", R2_ROOT);
    else if (!strcasecmp(container, "box") || !strcasecmp(container, "toy box"))
        snprintf(base, sizeof(base), "%s/room/box", R2_ROOT);
    else if (!strcasecmp(container, "pockets")) snprintf(base, sizeof(base), "%s/pockets", R2_ROOT);
    else if (!strcasecmp(container, "wallet")) snprintf(base, sizeof(base), "%s/pockets/wallet", R2_ROOT);
    else return 1; /* Other named containers remain database-only. */
    char slug[256];
    item_slug(name, slug, sizeof(slug));
    int n = snprintf(path, cap, "%s/%s.r2item", base, slug);
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
    if (found != 0 && food && !strcasecmp(food, "burger")) {
        if (fullness) *fullness = 100.0;
        if (energy) *energy = 10.0;
        found = 0;
    }
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
    if (food && !strcasecmp(food, "burger")) {
        if (ingredients && ingredients_cap && !*ingredients)
            snprintf(ingredients, ingredients_cap, "bread,beef,cheese");
        if (taste && taste_cap && !*taste)
            snprintf(taste, taste_cap, "savory,warm,salty");
    }
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
        if (n <= 0 || (size_t)n >= sizeof(entry)) continue;
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
    if (!strstr(out, "burger:")) {
        const char *guaranteed = "burger: fullness 100.0/100, energy bonus 10.0; ingredients: bread,beef,cheese; sensory description: savory,warm,salty (guaranteed fridge meal)\n";
        size_t add = strlen(guaranteed);
        if (len + add + 1 > cap) {
            char *grown = realloc(out, len + add + 1);
            if (grown) { out = grown; cap = len + add + 1; }
        }
        if (len + add + 1 <= cap) { memcpy(out + len, guaranteed, add + 1); len += add; }
    }
    if (!len) snprintf(out, cap, "No food metrics are configured in room/food_metrics.xml.\n");
    return out;
}

static void update_hunger_locked(double elapsed)
{
    sqlite3_stmt *st = NULL;
    double hunger = 0.0, satisfaction = 0.0, since_meal = 0.0, sleepiness = 0.0, energy = 100.0;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT hunger, seconds_since_meal, sleepiness, energy, satisfaction FROM r2_reality_self WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        hunger = sqlite3_column_double(st, 0);
        since_meal = sqlite3_column_double(st, 1);
        sleepiness = sqlite3_column_double(st, 2);
        energy = sqlite3_column_double(st, 3);
        satisfaction = sqlite3_column_double(st, 4);
    }
    if (st) sqlite3_finalize(st);
    hunger += elapsed * HUNGER_PER_SECOND;
    satisfaction -= elapsed * HUNGER_PER_SECOND;
    sleepiness += elapsed * HUNGER_PER_SECOND;
    energy -= elapsed * HUNGER_PER_SECOND;
    if (hunger > 100.0) hunger = 100.0;
    if (satisfaction < 0.0) satisfaction = 0.0;
    if (satisfaction > 150.0) satisfaction = 150.0;
    if (sleepiness > 100.0) sleepiness = 100.0;
    if (energy < 0.0) energy = 0.0;
    since_meal += elapsed;
    st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=?, seconds_since_meal=?, sleepiness=?, energy=?, satisfaction=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hunger);
        sqlite3_bind_double(st, 2, since_meal);
        sqlite3_bind_double(st, 3, sleepiness);
        sqlite3_bind_double(st, 4, energy);
        sqlite3_bind_double(st, 5, satisfaction);
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

/* Independent fridge database: fridge stock is not a room-object container. */
static int fridge_mirror_path(const char *name, char *path, size_t cap)
{
    char slug[256];
    item_slug(name, slug, sizeof(slug));
    int n = snprintf(path, cap, "%s/fridge/%s.r2item", R2_ROOT, slug);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
static void fridge_mirror_remove(const char *name)
{
    char path[2048];
    if (fridge_mirror_path(name, path, sizeof(path)) == 0) (void)unlink(path);
}
static void fridge_mirror_write(const char *name, const char *description, int quantity,
                                double fullness, double energy)
{
    char path[2048], temp[2100];
    if (fridge_mirror_path(name, path, sizeof(path)) != 0) return;
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    FILE *fp = fopen(temp, "w");
    if (!fp) return;
    int failed = fprintf(fp, "name=%s\ndescription=%s\nquantity=%d\ncontainer=fridge\nfullness=%.1f\nenergy=%.1f\n",
                         name, description ? description : "", quantity, fullness, energy) < 0;
    if (fclose(fp) != 0) failed = 1;
    if (!failed && rename(temp, path) != 0) failed = 1;
    if (failed) (void)unlink(temp);
}
static int fridge_seed_if_empty_locked(void)
{
    sqlite3_stmt *st = NULL;
    if (!fridge_db || sqlite3_prepare_v2(fridge_db, "SELECT COUNT(*) FROM r2_fridge_items", -1, &st, NULL) != SQLITE_OK) return -1;
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (count) return 0;
    const char *sql = "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) VALUES('burger','Guaranteed filling burger generated because the fridge was empty',1,100,10,'bread,beef,cheese','savory,warm,salty')";
    if (sqlite3_exec(fridge_db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    fridge_mirror_write("burger", "Guaranteed filling burger generated because the fridge was empty", 1, 100.0, 10.0);
    return 0;
}
static void fridge_sync_mirrors(void)
{
    if (!fridge_db) return;
    pthread_mutex_lock(&fridge_lock);
    char folder[1200];
    snprintf(folder, sizeof(folder), "%s/fridge", R2_ROOT);
    DIR *dir = opendir(folder);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            if (n >= 7 && !strcmp(entry->d_name + n - 7, ".r2item")) {
                char path[1600];
                snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
                (void)unlink(path);
            }
        }
        closedir(dir);
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fridge_db,
        "SELECT name,description,quantity,fullness,energy FROM r2_fridge_items ORDER BY name",
        -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *description = (const char *)sqlite3_column_text(st, 1);
            if (name) fridge_mirror_write(name, description, sqlite3_column_int(st, 2),
                sqlite3_column_double(st, 3), sqlite3_column_double(st, 4));
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&fridge_lock);
}

static int fridge_init(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/r2_fridge.db", R2_HOME);
    pthread_mutex_lock(&fridge_lock);
    int rc = sqlite3_open_v2(path, &fridge_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Fridge] Cannot open %s: %s\n", path, fridge_db ? sqlite3_errmsg(fridge_db) : "unknown error");
        if (fridge_db) sqlite3_close(fridge_db);
        fridge_db = NULL;
        pthread_mutex_unlock(&fridge_lock);
        return -1;
    }
    sqlite3_busy_timeout(fridge_db, 5000);
    sqlite3_exec(fridge_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", NULL, NULL, NULL);
    rc = sqlite3_exec(fridge_db,
        "CREATE TABLE IF NOT EXISTS r2_fridge_items(name TEXT PRIMARY KEY COLLATE NOCASE,description TEXT,quantity INTEGER NOT NULL CHECK(quantity>0),fullness REAL NOT NULL DEFAULT 100,energy REAL NOT NULL DEFAULT 10,ingredients TEXT,taste TEXT,updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_fridge_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO r2_fridge_meta(key,value) VALUES('schema_version','1');",
        NULL, NULL, NULL);
    if (rc == SQLITE_OK && fridge_seed_if_empty_locked() != 0) rc = SQLITE_ERROR;
    pthread_mutex_unlock(&fridge_lock);
    if (rc != SQLITE_OK) { fprintf(stderr, "[R2 Fridge] Could not initialize fridge schema or seed burger.\n"); return -1; }
    return 0;
}
void r2_fridge_shutdown(void)
{
    pthread_mutex_lock(&fridge_lock);
    if (fridge_db) {
        sqlite3_wal_checkpoint_v2(fridge_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(fridge_db);
        fridge_db = NULL;
    }
    pthread_mutex_unlock(&fridge_lock);
}
char *r2_fridge_list(void)
{
    if (!fridge_db) return NULL;
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    pthread_mutex_lock(&fridge_lock);
    int ok = fridge_seed_if_empty_locked() == 0;
    sqlite3_stmt *st = NULL;
    int rc = ok ? sqlite3_prepare_v2(fridge_db, "SELECT name,description,quantity,fullness,energy FROM r2_fridge_items ORDER BY name", -1, &st, NULL) : SQLITE_ERROR;
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *desc = (const char *)sqlite3_column_text(st, 1);
        char line[2048];
        int n = snprintf(line, sizeof(line), "%s — %s; quantity=%d; fullness=%.1f/100; energy=%.1f\n",
            name ? name : "unknown", desc ? desc : "", sqlite3_column_int(st, 2),
            sqlite3_column_double(st, 3), sqlite3_column_double(st, 4));
        if (n <= 0) continue;
        size_t add = (size_t)n;
        if (len + add + 1 > cap) {
            size_t next = cap * 2; while (next < len + add + 1) next *= 2;
            char *grown = realloc(out, next); if (!grown) break; out = grown; cap = next;
        }
        memcpy(out + len, line, add); len += add; out[len] = '\0';
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&fridge_lock);
    if (!len) snprintf(out, cap, "(fridge inventory unavailable)\n");
    return out;
}
char *r2_fridge_context(void)
{
    if (r2_reality_is_initialized() && !reality_is_home())
        return strdup("FRIDGE INACCESSIBLE: R2 is away from home. He must return home to access fridge stock.");
    char *items = r2_fridge_list();
    if (!items) return NULL;
    size_t cap = strlen(items) + 320;
    char *out = malloc(cap);
    if (out) snprintf(out, cap, "FRIDGE (separate persistent database; physically at home and accessible only while R2 is home):\n%sIf all fridge stock is consumed or removed, one filling burger (fullness 100/100) is generated automatically.", items);
    free(items);
    return out;
}
static int fridge_change_one(const char *food, char *description, size_t description_cap,
                             int *old_quantity, double *fullness, double *energy,
                             char *ingredients, size_t ingredients_cap, char *taste, size_t taste_cap)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fridge_db, "SELECT description,quantity,fullness,energy,ingredients,taste FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, food);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    const unsigned char *d = sqlite3_column_text(st, 0);
    if (description && description_cap) snprintf(description, description_cap, "%s", d ? (const char *)d : "");
    int qty = sqlite3_column_int(st, 1);
    if (old_quantity) *old_quantity = qty;
    if (fullness) *fullness = sqlite3_column_double(st, 2);
    if (energy) *energy = sqlite3_column_double(st, 3);
    const unsigned char *ing = sqlite3_column_text(st, 4), *flavor = sqlite3_column_text(st, 5);
    if (ingredients && ingredients_cap) snprintf(ingredients, ingredients_cap, "%s", ing ? (const char *)ing : "");
    if (taste && taste_cap) snprintf(taste, taste_cap, "%s", flavor ? (const char *)flavor : "");
    sqlite3_finalize(st);
    const char *sql = qty > 1
        ? "UPDATE r2_fridge_items SET quantity=quantity-1,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE"
        : "DELETE FROM r2_fridge_items WHERE name=? COLLATE NOCASE";
    st = NULL;
    if (sqlite3_prepare_v2(fridge_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    bind_text(st, 1, food);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}
static void fridge_restore_one(const char *food, const char *description, double fullness, double energy,
                              const char *ingredients, const char *taste)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fridge_db,
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) VALUES(?,?,1,?,?,?,?) ON CONFLICT(name) DO UPDATE SET quantity=quantity+1,updated_at=CURRENT_TIMESTAMP",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, food); bind_text(st, 2, description);
        sqlite3_bind_double(st, 3, fullness); sqlite3_bind_double(st, 4, energy);
        bind_text(st, 5, ingredients ? ingredients : ""); bind_text(st, 6, taste ? taste : "");
        (void)sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
}
int r2_fridge_add_item(const char *name, const char *description, int quantity,
                       double fullness, double energy, const char *ingredients, const char *taste)
{
    if (!name || !*name || strlen(name) > REALITY_MAX_TEXT || !fridge_db) return -1;
    if (quantity < 1) quantity = 1;
    if (fullness < 0.0 || fullness > 100.0) fullness = 100.0;
    if (energy < 0.0 || energy > 100.0) energy = 10.0;
    pthread_mutex_lock(&fridge_lock);
    int seed_rc = fridge_seed_if_empty_locked();
    sqlite3_stmt *st = NULL;
    int rc = seed_rc == 0 ? sqlite3_prepare_v2(fridge_db,
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(name) DO UPDATE SET description=excluded.description,quantity=r2_fridge_items.quantity+excluded.quantity,"
        "fullness=excluded.fullness,energy=excluded.energy,ingredients=excluded.ingredients,taste=excluded.taste,updated_at=CURRENT_TIMESTAMP",
        -1, &st, NULL) : SQLITE_ERROR;
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name); bind_text(st, 2, description ? description : "");
        sqlite3_bind_int(st, 3, quantity); sqlite3_bind_double(st, 4, fullness); sqlite3_bind_double(st, 5, energy);
        bind_text(st, 6, ingredients ? ingredients : ""); bind_text(st, 7, taste ? taste : "");
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(fridge_db, "SELECT quantity FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &q, NULL) == SQLITE_OK) {
            bind_text(q, 1, name);
            if (sqlite3_step(q) == SQLITE_ROW)
                fridge_mirror_write(name, description, sqlite3_column_int(q, 0), fullness, energy);
        }
        if (q) sqlite3_finalize(q);
    }
    pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_DONE ? 0 : -1;
}

int r2_fridge_take(const char *food)
{
    if (!food || !*food || !fridge_db || !r2_reality_is_initialized() || !reality_is_home()) return -1;
    char desc[REALITY_MAX_TEXT + 1] = {0}, ingredients[1024] = {0}, taste[1024] = {0};
    int qty = 0; double fullness = 100.0, energy = 10.0;
    pthread_mutex_lock(&fridge_lock);
    int rc = fridge_seed_if_empty_locked();
    if (rc == 0) rc = fridge_change_one(food, desc, sizeof(desc), &qty, &fullness, &energy,
                                         ingredients, sizeof(ingredients), taste, sizeof(taste));
    if (rc == 0) {
        if (qty == 1) fridge_mirror_remove(food);
        else fridge_mirror_write(food, desc, qty - 1, fullness, energy);
    }
    pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    if (r2_reality_add_item(food, desc, "pockets", 1) != 0) {
        pthread_mutex_lock(&fridge_lock); fridge_restore_one(food, desc, fullness, energy, ingredients, taste);
        fridge_mirror_write(food, desc, qty, fullness, energy); pthread_mutex_unlock(&fridge_lock);
        return -1;
    }
    pthread_mutex_lock(&fridge_lock); rc = fridge_seed_if_empty_locked(); pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 took %s from the fridge into his pockets.", food);
    snprintf(details, sizeof(details), "Food=%s; source=fridge database; destination=pockets; empty fridge generates a burger.", food);
    bridge_event("fridge_item_taken", summary, details, 1, 0);
    return 0;
}
int r2_reality_fridge_eat(const char *food, double fullness)
{
    if (!food || !*food || !fridge_db || !r2_reality_is_initialized() || !reality_is_home()) return -1;
    char desc[REALITY_MAX_TEXT + 1] = {0}, ingredients[1024] = {0}, taste[1024] = {0};
    int qty = 0; double stored_fullness = 100.0, energy = 10.0;
    pthread_mutex_lock(&fridge_lock);
    int rc = fridge_seed_if_empty_locked();
    if (rc == 0) rc = fridge_change_one(food, desc, sizeof(desc), &qty, &stored_fullness, &energy,
                                         ingredients, sizeof(ingredients), taste, sizeof(taste));
    if (rc == 0) {
        if (qty == 1) fridge_mirror_remove(food);
        else fridge_mirror_write(food, desc, qty - 1, stored_fullness, energy);
    }
    pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    if (fullness < 0.0) fullness = stored_fullness;
    rc = reality_eat_internal(food, fullness, 0, energy);
    if (rc != 0) {
        pthread_mutex_lock(&fridge_lock); fridge_restore_one(food, desc, stored_fullness, energy, ingredients, taste);
        fridge_mirror_write(food, desc, qty, stored_fullness, energy); pthread_mutex_unlock(&fridge_lock);
        return -1;
    }
    pthread_mutex_lock(&fridge_lock); rc = fridge_seed_if_empty_locked(); pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 ate %s directly from the fridge.", food);
    snprintf(details, sizeof(details), "Food=%s; source=fridge database; fullness=%.1f; fridge refills if emptied.", food, stored_fullness);
    bridge_event("fridge_food_consumed", summary, details, 1, 0);
    return 0;
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
static int legacy_column_exists(const char *table, const char *column)
{
    if (!table || !column) return 0;
    char sql[256];
    int n = snprintf(sql, sizeof(sql), "PRAGMA legacy.table_info(%s)", table);
    if (n <= 0 || (size_t)n >= sizeof(sql)) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(reality_db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && !strcasecmp((const char *)name, column)) { found = 1; break; }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

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

    int result = 0;
    /* Copy stable columns explicitly: old self-state schemas predate
       seconds_since_meal, so SELECT * would fail on those databases. */
    const struct { const char *table, *sql; } copies[] = {
        {"r2_reality_containers", "INSERT OR IGNORE INTO main.r2_reality_containers(name,kind,description,parent) SELECT name,kind,description,parent FROM legacy.r2_reality_containers"},
        {"r2_reality_meta", "INSERT OR IGNORE INTO main.r2_reality_meta(key,value) SELECT key,value FROM legacy.r2_reality_meta"},
        {"r2_reality_self_facts", "INSERT OR IGNORE INTO main.r2_reality_self_facts(key,value,evidence) SELECT key,value,evidence FROM legacy.r2_reality_self_facts"},
        {"r2_reality_ticks", "INSERT OR IGNORE INTO main.r2_reality_ticks(previous_tick,current_tick,elapsed_seconds) SELECT previous_tick,current_tick,elapsed_seconds FROM legacy.r2_reality_ticks"},
        {"r2_reality_objects", "INSERT OR IGNORE INTO main.r2_reality_objects(name,description,quantity,container,owner) SELECT name,description,quantity,container,owner FROM legacy.r2_reality_objects"}
    };
    for (size_t i = 0; i < sizeof(copies)/sizeof(copies[0]); ++i) {
        if (!legacy_table_exists(copies[i].table)) continue;
        if (exec_sql(copies[i].sql) != 0) { result = -1; break; }
    }
    if (result == 0 && legacy_table_exists("r2_reality_self")) {
        const char *since = legacy_column_exists("r2_reality_self", "seconds_since_meal") ? "seconds_since_meal" : "0";
        const char *sleep = legacy_column_exists("r2_reality_self", "sleepiness") ? "sleepiness" : "0";
        const char *energy = legacy_column_exists("r2_reality_self", "energy") ? "energy" : "100";
        const char *last = legacy_column_exists("r2_reality_self", "last_tick") ? "last_tick" : "0";
        char sql[1024];
        snprintf(sql, sizeof(sql),
            "INSERT OR REPLACE INTO main.r2_reality_self(id,hunger,seconds_since_meal,sleepiness,energy,last_tick) "
            "SELECT id,hunger,%s,%s,%s,%s FROM legacy.r2_reality_self",
            since, sleep, energy, last);
        result = exec_sql(sql);
    }
    if (result == 0 && legacy_table_exists("r2_reality_meta"))
        result = exec_sql("UPDATE main.r2_reality_meta SET value=(SELECT value FROM legacy.r2_reality_meta WHERE legacy.r2_reality_meta.key=main.r2_reality_meta.key) WHERE key IN (SELECT key FROM legacy.r2_reality_meta)");
    if (result == 0)
        result = exec_sql("INSERT INTO r2_reality_meta(key,value) VALUES('legacy_reality_migrated','yes') ON CONFLICT(key) DO UPDATE SET value='yes'");
    (void)sqlite3_exec(reality_db, "DETACH DATABASE legacy", NULL, NULL, NULL);
    if (result == 0)
        fprintf(stderr, "[R2 Reality] Legacy world/needs state migrated into r2_reality.db; r2_memory.db remains intact.\n");
    return result;
}

int r2_reality_init(void)
{
    pthread_mutex_lock(&reality_lock);
    if (reality_ready) { pthread_mutex_unlock(&reality_lock); return 0; }
    if (make_room_dirs() != 0) { pthread_mutex_unlock(&reality_lock); return -1; }

    char reality_path[1200];
    snprintf(reality_path, sizeof(reality_path), "%s/r2_reality.db", R2_HOME);
    int rc = sqlite3_open_v2(reality_path, &reality_db,
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
        "INSERT OR IGNORE INTO r2_reality_meta(key,value) VALUES('current_location','home');"
        "INSERT OR IGNORE INTO r2_reality_meta(key,value) VALUES('initial_cash_seeded','0');"
        "CREATE TABLE IF NOT EXISTS r2_reality_item_memory (id INTEGER PRIMARY KEY AUTOINCREMENT, item_name TEXT NOT NULL COLLATE NOCASE, description TEXT, exact_quantity INTEGER, approximate_quantity INTEGER, precision TEXT NOT NULL DEFAULT 'exact' CHECK(precision IN ('exact','approximate','vague')), collected_at INTEGER NOT NULL, last_decay_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS r2_reality_item_memory_age_idx ON r2_reality_item_memory(collected_at);"
        "CREATE TABLE IF NOT EXISTS r2_food_experiences (id INTEGER PRIMARY KEY AUTOINCREMENT, food_name TEXT NOT NULL COLLATE NOCASE, ingredients TEXT, fullness REAL NOT NULL, energy_bonus REAL NOT NULL, satisfaction INTEGER, notes TEXT, eaten_at INTEGER NOT NULL, rated_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS r2_food_experiences_food_idx ON r2_food_experiences(food_name,eaten_at);"
        "CREATE TABLE IF NOT EXISTS r2_food_ingredient_preferences (ingredient TEXT PRIMARY KEY COLLATE NOCASE, satisfaction_sum REAL NOT NULL DEFAULT 0, rating_count INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS r2_food_preferences (food_name TEXT PRIMARY KEY COLLATE NOCASE, satisfaction_sum REAL NOT NULL DEFAULT 0, rating_count INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS r2_food_experience_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO r2_food_experience_meta(key,value) VALUES('schema_version','1');"
        "CREATE TABLE IF NOT EXISTS r2_reality_self (id INTEGER PRIMARY KEY CHECK(id=1), hunger REAL NOT NULL DEFAULT 0, satisfaction REAL NOT NULL DEFAULT 100, seconds_since_meal REAL NOT NULL DEFAULT 0, sleepiness REAL NOT NULL DEFAULT 0, energy REAL NOT NULL DEFAULT 100, last_tick INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_reality_self(id,hunger,seconds_since_meal,sleepiness,energy,last_tick) VALUES(1,0,0,0,100,strftime('%s','now'));"
        "CREATE TABLE IF NOT EXISTS r2_reality_self_facts (key TEXT PRIMARY KEY, value TEXT NOT NULL, evidence TEXT, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_money_account (id INTEGER PRIMARY KEY CHECK(id=1),cash_cents INTEGER NOT NULL DEFAULT 0 CHECK(cash_cents>=0),bank_cents INTEGER NOT NULL DEFAULT 0 CHECK(bank_cents>=0),updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_money_account(id,cash_cents,bank_cents) VALUES(1,0,0);"
        "CREATE TABLE IF NOT EXISTS r2_tv_state (id INTEGER PRIMARY KEY CHECK(id=1), power INTEGER NOT NULL DEFAULT 0 CHECK(power IN (0,1)), source_kind TEXT NOT NULL DEFAULT 'input', source_value INTEGER NOT NULL DEFAULT 1, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_tv_state(id,power,source_kind,source_value) VALUES(1,0,'input',1);"
        "CREATE TABLE IF NOT EXISTS r2_tv_devices (name TEXT PRIMARY KEY COLLATE NOCASE, connection_kind TEXT NOT NULL CHECK(connection_kind IN ('input','rf')), port INTEGER NOT NULL, connected INTEGER NOT NULL DEFAULT 1 CHECK(connected IN (0,1)), updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, CHECK((connection_kind='input' AND port BETWEEN 2 AND 4) OR (connection_kind='rf' AND port BETWEEN 2 AND 13)));"
        "CREATE UNIQUE INDEX IF NOT EXISTS r2_tv_one_device_per_port ON r2_tv_devices(connection_kind,port) WHERE connected=1;"
        "CREATE TABLE IF NOT EXISTS r2_tv_vcr_tapes (media_path TEXT PRIMARY KEY, position_seconds REAL NOT NULL DEFAULT 0 CHECK(position_seconds>=0), updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_tv_vcr_state (id INTEGER PRIMARY KEY CHECK(id=1), cassette_path TEXT, transport TEXT NOT NULL DEFAULT 'stop' CHECK(transport IN ('play','pause','stop')), updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(cassette_path) REFERENCES r2_tv_vcr_tapes(media_path));"
        "INSERT OR IGNORE INTO r2_tv_vcr_state(id,cassette_path,transport) VALUES(1,NULL,'stop');"
        "CREATE TABLE IF NOT EXISTS r2_reality_containers (name TEXT PRIMARY KEY, kind TEXT NOT NULL, description TEXT, parent TEXT, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_reality_objects (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE UNIQUE, description TEXT, quantity INTEGER NOT NULL DEFAULT 1 CHECK(quantity>0), container TEXT NOT NULL DEFAULT 'room', owner TEXT NOT NULL DEFAULT 'R2', created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(container) REFERENCES r2_reality_containers(name));"
        "CREATE INDEX IF NOT EXISTS r2_reality_objects_container_idx ON r2_reality_objects(container);"
        "CREATE TABLE IF NOT EXISTS r2_reality_ticks (id INTEGER PRIMARY KEY AUTOINCREMENT, previous_tick INTEGER NOT NULL, current_tick INTEGER NOT NULL, elapsed_seconds INTEGER NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_reality_containers(name,kind,description,parent) VALUES"
        "('world','world','The simulated world root',''),"
        "('outside','environment','The area outside R2''s home','world'),"
        "('home','location','R2''s home; room and fridge are located here','world'),"
        "('R2','person','R2 and his portable belongings','world'),"
        "('room','room','R2''s room','home'),"
        "('shelf','surface','The shelf in R2''s room','room'),"
        "('box','container','The general storage box in R2''s room','room'),"
        "('pockets','inventory','R2''s portable pockets','R2'),"
        "('wallet','inventory','R2''s wallet inside his pockets','pockets'),"
        "('fridge','container','The fridge in R2''s home','home');"
        /* The television is initial room furnishing, not a purchased item.
           INSERT OR IGNORE preserves any later location chosen by R2. */
        "INSERT OR IGNORE INTO r2_reality_objects(name,description,quantity,container,owner) "
        "VALUES('TV','CRT television with a built-in VCR and expandable external inputs.',1,'room','R2');";
    if (exec_sql(schema) != 0) {
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    /* Upgrade old persistent databases in place; preserve existing need state. */
    int has_satisfaction = 0;
    sqlite3_stmt *columns = NULL;
    if (sqlite3_prepare_v2(reality_db, "PRAGMA table_info(r2_reality_self)", -1, &columns, NULL) == SQLITE_OK) {
        while (sqlite3_step(columns) == SQLITE_ROW) {
            const unsigned char *column_name = sqlite3_column_text(columns, 1);
            if (column_name && !strcasecmp((const char *)column_name, "satisfaction")) {
                has_satisfaction = 1;
                break;
            }
        }
    }
    if (columns) sqlite3_finalize(columns);
    if (!has_satisfaction && exec_sql(
        "ALTER TABLE r2_reality_self ADD COLUMN satisfaction REAL NOT NULL DEFAULT 100") != 0) {
        fprintf(stderr, "[R2 Reality] Could not add the satisfaction need to persistent state.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (migrate_legacy_reality() != 0) {
        fprintf(stderr, "[R2 Reality] Legacy migration failed; refusing to discard old state.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    /* Upgrade existing persistent containers without resetting any inventory. */
    if (exec_sql(
        "INSERT OR IGNORE INTO r2_reality_containers(name,kind,description,parent) VALUES"
        "('world','world','The simulated world root',''),"
        "('outside','environment','The area outside R2''s home','world'),"
        "('home','location','R2''s home; room and fridge are located here','world'),"
        "('R2','person','R2 and his portable belongings','world'),"
        "('room','room','R2''s room','home'),"
        "('shelf','surface','The shelf in R2''s room','room'),"
        "('box','container','The general storage box in R2''s room','room'),"
        "('pockets','inventory','R2''s portable pockets','R2'),"
        "('wallet','inventory','R2''s wallet inside his pockets','pockets'),"
        "('fridge','container','The fridge in R2''s home','home');"
        "UPDATE r2_reality_objects SET container='box' WHERE lower(container)='toy box';"
        "UPDATE r2_reality_containers SET parent='world' WHERE name IN ('outside','home','R2');"
        "UPDATE r2_reality_containers SET parent='home' WHERE name IN ('room','fridge');"
        "UPDATE r2_reality_containers SET parent='room' WHERE name IN ('shelf','box');"
        "UPDATE r2_reality_containers SET parent='R2' WHERE name='pockets';"
        "UPDATE r2_reality_containers SET parent='pockets' WHERE name='wallet';"
        "DELETE FROM r2_reality_containers WHERE lower(name)='toy box';") != 0) {
        fprintf(stderr, "[R2 Reality] Could not normalize persistent container hierarchy.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    sqlite3_stmt *seed_st = NULL;
    int cash_seeded = 0;
    if (sqlite3_prepare_v2(reality_db, "SELECT value FROM r2_reality_meta WHERE key='initial_cash_seeded'",
                           -1, &seed_st, NULL) == SQLITE_OK && sqlite3_step(seed_st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(seed_st, 0);
        cash_seeded = v && !strcmp((const char *)v, "1");
    }
    if (seed_st) sqlite3_finalize(seed_st);
    if (!cash_seeded && exec_sql(
        "UPDATE r2_money_account SET cash_cents=cash_cents+500 WHERE id=1;"
        "INSERT INTO r2_reality_meta(key,value) VALUES('initial_cash_seeded','1') "
        "ON CONFLICT(key) DO UPDATE SET value='1';") != 0) {
        fprintf(stderr, "[R2 Reality] Could not seed initial $5 carried cash.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (fridge_init() != 0) {
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    fridge_sync_mirrors();

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
    sqlite3_stmt *loc_st = NULL;
    reality_home_state = 1;
    if (sqlite3_prepare_v2(reality_db, "SELECT value FROM r2_reality_meta WHERE key='current_location'",
                           -1, &loc_st, NULL) == SQLITE_OK && sqlite3_step(loc_st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(loc_st, 0);
        if (v && (!strcasecmp((const char *)v, "outside") || !strcasecmp((const char *)v, "away")))
            reality_home_state = 0;
    }
    if (loc_st) sqlite3_finalize(loc_st);
    reality_ready = 1;
    pthread_mutex_unlock(&reality_lock);
    if (r2_addiction_init() != 0)
        fprintf(stderr, "[R2 Reality] Addiction/preference history is unavailable; core world remains available.\n");
    sync_room_mirrors();

    sqlite3_int64 carried_cash = 0, bank_cash = 0;
    pthread_mutex_lock(&reality_lock);
    (void)money_read_locked(&carried_cash, &bank_cash);
    pthread_mutex_unlock(&reality_lock);
    money_mirror_write(carried_cash, bank_cash);
    bridge_event("reality_engine_started", "R2's persistent reality engine started.",
        "Self-continuity and world-continuity are stored in dedicated r2_reality.db; Life Log and searchable memory remain in r2_memory.db. Physical hierarchy: room/shelf/box and piggybank under room; portable pockets/wallet and fixed home fridge under R2_Home; factual events in log/log.txt.", 1, 0);
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
    r2_fridge_shutdown();
    reality_ready = 0;
    pthread_mutex_unlock(&reality_lock);
    r2_addiction_shutdown();
}

int r2_reality_is_initialized(void)
{
    pthread_mutex_lock(&reality_lock);
    int ready = reality_ready;
    pthread_mutex_unlock(&reality_lock);
    return ready;
}
int r2_reality_set_location(int home)
{
    if (!r2_reality_is_initialized()) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_reality_meta(key,value) VALUES('current_location',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, home ? "home" : "outside");
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc == SQLITE_DONE) __atomic_store_n(&reality_home_state, home ? 1 : 0, __ATOMIC_RELEASE);
    return rc == SQLITE_DONE ? 0 : -1;
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
    if (!container_accessible(c))
        return strdup("Inaccessible while R2 is away from home; only pockets and wallet travel with him.");
    char sql[] = "SELECT name,description,printf('x%d in %s',quantity,container) FROM r2_reality_objects WHERE container=? ORDER BY name COLLATE NOCASE";
    return query_text(sql, c);
}

char *r2_reality_room_look(void)
{
    if (r2_reality_tick() != 0) return NULL;
    if (!reality_is_home()) {
        char *pockets = r2_reality_list("pockets");
        char *wallet = r2_reality_list("wallet");
        if (!pockets || !wallet) { free(pockets); free(wallet); return NULL; }
        size_t n = strlen(pockets) + strlen(wallet) + 256;
        char *out = malloc(n);
        if (out) snprintf(out, n,
            "R2 IS AWAY FROM HOME\nHis room, shelf, storage box, and fridge are physically inaccessible until he returns home.\n"
            "PORTABLE INVENTORY\nPockets: %sWallet: %s", pockets, wallet);
        free(pockets); free(wallet);
        return out;
    }
    char *room = r2_reality_list("room");
    char *shelf = r2_reality_list("shelf");
    char *box = r2_reality_list("box");
    char *pockets = r2_reality_list("pockets");
    char *wallet = r2_reality_list("wallet");
    char *named = query_text("SELECT c.name,group_concat(o.name, ', '),printf('%d items',count(o.id)) FROM r2_reality_containers c LEFT JOIN r2_reality_objects o ON o.container=c.name WHERE c.parent='room' AND c.name NOT IN ('room','shelf','box') GROUP BY c.name ORDER BY c.name",NULL);
    if (!room || !shelf || !box || !pockets || !wallet || !named) {
        free(room); free(shelf); free(box); free(pockets); free(wallet); free(named); return NULL;
    }
    size_t n = strlen(room)+strlen(shelf)+strlen(box)+strlen(pockets)+strlen(wallet)+strlen(named)+720;
    char *out = malloc(n);
    if (out) snprintf(out,n,
        "R2'S ROOM\nRoom: %sShelf: %sBox: %sNamed containers in the room and their contents:\n%s"
        "SELF INVENTORY (outside the room)\nPockets: %sWallet (inside pockets): %s",
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
    double hunger=0, satisfaction=0, since=0, sleepiness=0, energy=100;
    if (sqlite3_prepare_v2(reality_db,"SELECT hunger,seconds_since_meal,sleepiness,energy,satisfaction FROM r2_reality_self WHERE id=1",-1,&st,NULL)==SQLITE_OK &&
        sqlite3_step(st)==SQLITE_ROW) {
        hunger=sqlite3_column_double(st,0); since=sqlite3_column_double(st,1);
        sleepiness=sqlite3_column_double(st,2); energy=sqlite3_column_double(st,3); satisfaction=sqlite3_column_double(st,4);
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
    snprintf(out,4096,"SELF CONTINUITY\nSatisfaction: %5.1f/150\nHunger: %04.1f/100 (%s)\nTime since meal: %.1f hours\nSleepiness: %.1f/100\nEnergy: %.1f/100\nModeled world time elapsed: %lld days, %lld hours\nObjects tracked in the world: %d\nHunger rises 0.1 and satiety falls 0.1 every 4.75 seconds of modeled elapsed time; satiety caps at 150/150.\nAfter 72 hours without food, prolonged starvation is recorded; needs do not magically reset on restart.\n",
        satisfaction,hunger,hstate,since/3600.0,sleepiness,energy,
        (long long)(world_elapsed/86400),(long long)((world_elapsed%86400)/3600),count);
    return out;
}

/* libfaketime intentionally gives R2 a private 1999 clock. Age is a
 * real elapsed-time calculation, so read the host boot epoch and kernel
 * uptime rather than comparing the file's real birth time with R2's fake
 * wall clock. Both values come from procfs and are unaffected by LD_PRELOAD. */
static sqlite3_int64 real_epoch_seconds(void)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    char line[256];
    long long boot_epoch = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "btime %lld", &boot_epoch) == 1) break;
    }
    fclose(fp);
    if (boot_epoch <= 0) return 0;

    fp = fopen("/proc/uptime", "r");
    if (!fp) return 0;
    double uptime = 0.0;
    int parsed = fscanf(fp, "%lf", &uptime);
    fclose(fp);
    if (parsed != 1 || !isfinite(uptime) || uptime < 0.0) return 0;
    return (sqlite3_int64)boot_epoch + (sqlite3_int64)uptime;
}

static char *origin_age_context(void)
{
    char path[1400]; snprintf(path,sizeof(path),"%s/r2_original_conversation.txt",R2_HOME);
    sqlite3_int64 started=0; pthread_mutex_lock(&reality_lock); sqlite3_stmt *st=NULL;
    if(reality_db&&sqlite3_prepare_v2(reality_db,"SELECT value FROM r2_reality_meta WHERE key='original_conversation_birth_time'",-1,&st,NULL)==SQLITE_OK&&sqlite3_step(st)==SQLITE_ROW){
        const unsigned char *v=sqlite3_column_text(st,0);if(v)started=(sqlite3_int64)strtoll((const char*)v,NULL,10);}
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if(started<=0){
        struct statx sx;memset(&sx,0,sizeof(sx));
        if(statx(AT_FDCWD,path,AT_STATX_SYNC_AS_STAT,STATX_BTIME,&sx)==0&&
           (sx.stx_mask&STATX_BTIME)&&sx.stx_btime.tv_sec>0)
            started=(sqlite3_int64)sx.stx_btime.tv_sec;
        if(started>0){char v[64];snprintf(v,sizeof(v),"%lld",(long long)started);pthread_mutex_lock(&reality_lock);st=NULL;
            if(reality_db&&sqlite3_prepare_v2(reality_db,"INSERT OR IGNORE INTO r2_reality_meta(key,value) VALUES('original_conversation_birth_time',?)",-1,&st,NULL)==SQLITE_OK){bind_text(st,1,v);(void)sqlite3_step(st);}
            if (st) sqlite3_finalize(st);
            pthread_mutex_unlock(&reality_lock);
        }
    }
    if(started<=0){
        if(access(path,F_OK)==0)
            return strdup("AGE: original conversation file exists, but its filesystem creation time is unavailable; age is not guessed.");
        return strdup("AGE: original conversation file is not present yet; age is not established.");
    }
    sqlite3_int64 now = real_epoch_seconds();
    if (now <= 0)
        return strdup("AGE: the host's real elapsed time is unavailable; age is not guessed from R2's private clock.");
    sqlite3_int64 days = now > started ? (now - started) / 86400 : 0;
    char out[256];
    if(days<30)snprintf(out,sizeof(out),"AGE: about %lld days since the original conversation file was created.",(long long)days);
    else if(days<365){double m=(double)days/30.436875;if(m>=5.5&&m<=6.5)snprintf(out,sizeof(out),"AGE: about half a year old (%lld days).",(long long)days);else snprintf(out,sizeof(out),"AGE: about %d months old, measured from the original conversation file's creation time.",(int)(m+0.5));}
    else{int y=(int)(days/365.2425),m=(int)(((double)days-y*365.2425)/30.436875+0.5);if(m>=12){y++;m=0;}
        if(m)snprintf(out,sizeof(out),"AGE: about %d years and %d months old, measured from the original conversation file's creation time.",y,m);
        else snprintf(out,sizeof(out),"AGE: about %d years old, measured from the original conversation file's creation time.",y);}
    return strdup(out);
}

char *r2_reality_context(void)
{
    char *status = r2_reality_status();
    char *room = r2_reality_room_look();
    char *facts = query_text("SELECT key,value,evidence FROM r2_reality_self_facts ORDER BY updated_at DESC", NULL);
    char *items_memory = query_text(
        "WITH detailed AS (SELECT item_name,CASE precision WHEN 'exact' THEN printf('remembers collecting exactly %d',exact_quantity) ELSE printf('remembers collecting about %d',approximate_quantity) END AS memory,CASE precision WHEN 'exact' THEN printf('%d days ago',MAX(0,(strftime('%s','now')-collected_at)/86400)) ELSE printf('about %d weeks ago',MAX(1,ROUND((strftime('%s','now')-collected_at)/604800.0))) END AS timing,collected_at FROM r2_reality_item_memory WHERE precision IN ('exact','approximate')), vague AS (SELECT item_name,'remembers collecting some; exact quantity and timing have faded' AS memory,'older vague memory' AS timing,MAX(collected_at) AS collected_at FROM r2_reality_item_memory WHERE precision='vague' GROUP BY item_name) SELECT item_name,memory,timing FROM (SELECT * FROM detailed UNION ALL SELECT * FROM vague) ORDER BY collected_at DESC LIMIT 80", NULL);
    char *food_preferences = query_text("SELECT ingredient,printf('average enjoyment rating %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_ingredient_preferences WHERE rating_count>0 ORDER BY satisfaction_sum*1.0/rating_count DESC", NULL);
    char *food_experiences = query_text("SELECT food_name,printf('enjoyment rating %+d/2',satisfaction),notes FROM r2_food_experiences WHERE satisfaction IS NOT NULL ORDER BY eaten_at DESC LIMIT 20", NULL);
    char *foods = food_metrics_context();
    char *fridge=r2_fridge_context(); char *age=origin_age_context(); char *money=r2_reality_money_context();
    char *recent_log=r2_log_recent(5);
    char *visual_memory=r2_visual_recent(2);
    if(!recent_log) recent_log=strdup("Recent Life Log evidence is unavailable.");
    if(!visual_memory) visual_memory=strdup("No stored visual observations are available.");
    if(!status||!room||!facts||!items_memory||!food_preferences||!food_experiences||!foods||!fridge||!age||!money||!recent_log||!visual_memory){
        free(status);free(room);free(fridge);free(facts);free(items_memory);free(food_preferences);free(food_experiences);free(foods);free(age);free(money);free(recent_log);free(visual_memory);return NULL;}
    size_t n=strlen(status)+strlen(room)+strlen(fridge)+strlen(facts)+strlen(items_memory)+strlen(food_preferences)+strlen(food_experiences)+strlen(foods)+strlen(age)+strlen(money)+strlen(recent_log)+strlen(visual_memory)+6000;
    char *out=malloc(n);
    if(out)snprintf(out,n,
        "PERSISTENT REALITY CONTEXT (authoritative database state; do not invent changes):\n"
        "%s\n%s\n%s\n%s\n%s\nRECENT LIFE LOG EVIDENCE (events and observations; not automatically current):\n%s\nRECENT VISUAL EXPERIENCES (actual stored observations; not necessarily current):\n%s\nSELF-CONTINUITY FACTS:\n%s\nCOLLECTION MEMORIES (precision intentionally fades; not current inventory):\n%s\nLEARNED FOOD/INGREDIENT PREFERENCES (subjective scores):\n%s\nRECENT RATED FOOD EXPERIENCES:\n%s\nAVAILABLE FOOD METRICS:\n%s\n"
        "WORLD ACTIONS: Put one action on its own line. Use [WORLD] tv_status, tv_power|on/off, tv_input|1..4, tv_tune|2..13, tv_connect|device name|input/RF|port, or tv_disconnect|device name for the CRT television; power/source/attention are separate, and empty RF channels show no signal (no snow-show yet). Use [WORLD] look to inspect the room; "
        "[WORLD] add|name|description|container|quantity to collect/add a stack (adds to an existing stack and records a collection memory); "
        "[WORLD] move|name|container to relocate an existing item without counting a new collection; [WORLD] remove|name to remove it; "
        "[WORLD] eat|food|fullness_points (or auto for XML) to update hunger; "
        "[WORLD] sleep|hours to advance sleep recovery and trigger a private dream simulation; "
        "[WORLD] dream|description to record a reported dream; "
        "[WORLD] ratefood|food|-2..2|reason to rate the most recent unrated eating experience. "
        "[WORLD] self|key|value|evidence to record a self-state fact. "
        "[WORLD] fridge|look to inspect fridge stock; [WORLD] fridge_take|food to move one item into pockets; [WORLD] fridge_eat|food to eat directly from fridge stock without consuming a similarly named pocket item; [WORLD] fridge_store|food to move a tracked inventory item into fridge stock. "
        "The fridge is physically at home, outside the room (a sibling of room/ and pockets/), and is accessible only while R2 is home; it automatically generates one burger with fullness 100/100 whenever all stock is gone. Its stock is stored in a separate r2_fridge.db. " 
        "Food metrics live in room/food_metrics.xml; each food can define fullness, energy, ingredients (comma-separated), and taste (sensory description). Use only listed metrics and auto rather than guessing. "
        "Containers: room, shelf, and one storage box; pockets are portable anywhere, the fridge is a fixed home object, and the wallet is inside pockets. While away from home, R2 may access only pockets and wallet; room objects and fridge stock require returning home. The old toy box is the same box and is normalized into it. Named containers can be created by moving an item to a new container name. " 
        "Food fullness points are modeled values, not measured biological facts. Sleep advances hunger and world time. "
        "Dreams are stored as simulated reports, not waking facts. Ask before moving or deleting a user's important item. "
        "CHOICE LAB: Consider any possible option, action, object, or decision; there is no fixed menu. Compare alternatives using needs, memories, self-facts, preferences, and available evidence. "
        "ALTERNATE-SELF / CHOICE LAB: consider any possible option or action, not a fixed menu. [ALTERNATE_LIST] lists stored what-if branches; [ALTERNATE_SHOW] id inspects one; [ALTERNATE_COMPARE] id id compares two. To persist a useful hypothetical, emit [ALTERNATE_CREATE] name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID [END ALTERNATE_CREATE]. Use it when the user asks to save a what-if or when comparing distinct plausible futures would help; a branch is a hypothesis, never a factual event. Use [ALTERNATE_RETAIN] id or [ALTERNATE_DISCARD] id only when explicitly asked. Never promote predicted outcomes into real memories; retained hypotheses remain hypotheses. "
        "SENSORY COUNTERFACTUALS: Imagine what a hypothetical view, sound, taste, smell, or touch might reveal without activating Eyes/Ears, changing the world, or claiming an actual sensation. Label imagined details as predictions. Ask 'what if I looked over here?' or 'would I still like X if I knew Y?' Use recent Life Log observations, stored visual experiences, learned preferences, and food/ingredient/taste metrics; distinguish evidence from guesses and update preferences only after real experience or explicit feedback. The current Ears subsystem can log audio source/sample activity without interpreting sound content; do not claim to recognize a sound unless an actual description or transcript exists. "
        "MONEY: cash is carried money; bank is stored in room/piggybank/. When the user explicitly says R2 receives money, use [WORLD] money_receive|amount; use [WORLD] money_deposit|amount, [WORLD] money_withdraw|amount, or [WORLD] buy|item|price|description|container. If R2 accepts an offer to buy something, transact only when its price is explicitly known; never invent a price or stock, and ask for the price if missing. A purchase goes to the named destination; use pockets for goods to carry to the fridge, and room for furniture placed in the room. A free gift is not a purchase. No stock, starting funds, or prices are hardcoded. "
        "Do not claim an action succeeded unless the action result confirms it.",
        status, room, fridge, age, money, recent_log, visual_memory, facts, items_memory, food_preferences, food_experiences, foods);
    free(status); free(room); free(fridge); free(facts); free(items_memory); free(food_preferences); free(food_experiences); free(foods); free(age); free(money); free(recent_log); free(visual_memory);
    return out;
}

static void canonical_container_name(const char *input, char *out, size_t cap)
{
    if (!input || !*input) input = "room";
    if (!strcasecmp(input, "toy box")) input = "box";
    else if (!strcasecmp(input, "pocket")) input = "pockets";
    if (cap) snprintf(out, cap, "%s", input);
}

static const char *container_parent_name(const char *container)
{
    if (!strcasecmp(container, "room") || !strcasecmp(container, "fridge")) return "home";
    if (!strcasecmp(container, "pockets")) return "R2";
    if (!strcasecmp(container, "wallet")) return "pockets";
    if (!strcasecmp(container, "home") || !strcasecmp(container, "R2")) return "world";
    if (!strcasecmp(container, "outside") || !strcasecmp(container, "world")) return "";
    return "room";
}

static int container_is_inventory(const char *container)
{
    return !strcasecmp(container, "pockets") || !strcasecmp(container, "wallet");
}

int r2_reality_add_item(const char *name,const char *description,const char *container,int quantity)
{
    if(!name || !*name || strlen(name)>REALITY_MAX_TEXT || (description && strlen(description)>REALITY_MAX_TEXT)) return -1;
    char canonical_container[REALITY_MAX_TEXT + 1];
    canonical_container_name(container, canonical_container, sizeof(canonical_container));
    container = canonical_container;
    if (!strcasecmp(container, "fridge")) {
        if (!reality_is_home()) return -1;
        char ingredients[1024] = {0}, taste[1024] = {0};
        food_attributes(name, ingredients, sizeof(ingredients), taste, sizeof(taste));
        double fullness = 100.0, energy = 10.0;
        (void)food_metric(name, &fullness, &energy);
        return r2_fridge_add_item(name, description, quantity, fullness, energy, ingredients, taste);
    }
    if(quantity<1) quantity=1;
    if(!r2_reality_is_initialized() || !container_accessible(container)) return -1;
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    int total_quantity = quantity;
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
    if (*old_container && !container_accessible(old_container)) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,container_is_inventory(container)?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,container_parent_name(container));rc=sqlite3_step(st);}
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
    if (rc == SQLITE_DONE) {
        st = NULL;
        if (sqlite3_prepare_v2(reality_db, "SELECT quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &st, NULL) == SQLITE_OK) {
            bind_text(st, 1, name);
            if (sqlite3_step(st) == SQLITE_ROW) total_quantity = sqlite3_column_int(st, 0);
        }
        if (st) sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&reality_lock);
    if(rc!=SQLITE_DONE) return -1;
    if (*old_container) mirror_remove(name, old_container);
    if (mirror_write(name, description, total_quantity, container) != 0)
        fprintf(stderr, "[R2 Reality] Database saved, but room mirror file could not be updated for '%s'.\n", name);
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 recorded item '%.200s' in %.200s.",name,container);
    snprintf(details,sizeof(details),"Item=%.300s; description=%.1200s; container=%.200s; quantity=%d",name,description?description:"",container,quantity);
    bridge_event("object_added",summary,details,1,1);
    return 0;
}

int r2_reality_move_item(const char *name,const char *container)
{
    if(!name||!*name||!container||!*container||strlen(name)>REALITY_MAX_TEXT||strlen(container)>REALITY_MAX_TEXT||!r2_reality_is_initialized()) return -1;
    char canonical_container[REALITY_MAX_TEXT + 1];
    canonical_container_name(container, canonical_container, sizeof(canonical_container));
    container = canonical_container;
    if (!strcasecmp(container, "fridge")) {
        char description[REALITY_MAX_TEXT + 1] = {0}, old_container[REALITY_MAX_TEXT + 1] = {0};
        int quantity = 0;
        pthread_mutex_lock(&reality_lock);
        sqlite3_stmt *item = NULL;
        if (sqlite3_prepare_v2(reality_db, "SELECT description,container,quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &item, NULL) == SQLITE_OK) {
            bind_text(item, 1, name);
            if (sqlite3_step(item) == SQLITE_ROW) {
                const unsigned char *d = sqlite3_column_text(item, 0), *c = sqlite3_column_text(item, 1);
                if (d) snprintf(description, sizeof(description), "%s", (const char *)d);
                if (c) snprintf(old_container, sizeof(old_container), "%s", (const char *)c);
                quantity = sqlite3_column_int(item, 2);
            }
        }
        if (item) sqlite3_finalize(item);
        pthread_mutex_unlock(&reality_lock);
        if (quantity < 1 || !container_accessible(old_container) || !container_accessible(container)) return -1;
        char ingredients[1024] = {0}, taste[1024] = {0};
        food_attributes(name, ingredients, sizeof(ingredients), taste, sizeof(taste));
        double fullness = 100.0, energy = 10.0;
        (void)food_metric(name, &fullness, &energy);
        int prior_fridge_quantity = 0;
        pthread_mutex_lock(&fridge_lock);
        sqlite3_stmt *prior_fridge = NULL;
        if (sqlite3_prepare_v2(fridge_db, "SELECT quantity FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &prior_fridge, NULL) == SQLITE_OK) {
            bind_text(prior_fridge, 1, name);
            if (sqlite3_step(prior_fridge) == SQLITE_ROW) prior_fridge_quantity = sqlite3_column_int(prior_fridge, 0);
        }
        if (prior_fridge) sqlite3_finalize(prior_fridge);
        pthread_mutex_unlock(&fridge_lock);
        if (r2_fridge_add_item(name, description, quantity, fullness, energy, ingredients, taste) != 0) return -1;
        if (r2_reality_remove_item(name) != 0) {
            pthread_mutex_lock(&fridge_lock);
            sqlite3_stmt *rollback = NULL;
            const char *rollback_sql = prior_fridge_quantity > 0
                ? "UPDATE r2_fridge_items SET quantity=?,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE"
                : "DELETE FROM r2_fridge_items WHERE name=? COLLATE NOCASE";
            if (sqlite3_prepare_v2(fridge_db, rollback_sql, -1, &rollback, NULL) == SQLITE_OK) {
                if (prior_fridge_quantity > 0) {
                    sqlite3_bind_int(rollback, 1, prior_fridge_quantity);
                    bind_text(rollback, 2, name);
                } else bind_text(rollback, 1, name);
                (void)sqlite3_step(rollback);
            }
            if (rollback) sqlite3_finalize(rollback);
            if (prior_fridge_quantity > 0)
                fridge_mirror_write(name, description, prior_fridge_quantity, fullness, energy);
            else fridge_mirror_remove(name);
            (void)fridge_seed_if_empty_locked();
            pthread_mutex_unlock(&fridge_lock);
            return -1;
        }
        char summary[512], details[1024];
        snprintf(summary, sizeof(summary), "R2 stored %s in the fridge.", name);
        snprintf(details, sizeof(details), "Object=%s; source container=%s; quantity=%d; destination=separate fridge database.", name, old_container, quantity);
        bridge_event("fridge_item_stored", summary, details, 1, 0);
        return 0;
    }
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
    if (!*old_container || !container_accessible(old_container) || !container_accessible(container)) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,"INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",-1,&st,NULL);
    if(rc==SQLITE_OK){bind_text(st,1,container);bind_text(st,2,container_is_inventory(container)?"inventory":"container");bind_text(st,3,"Persistent object container");bind_text(st,4,container_parent_name(container));rc=sqlite3_step(st);}
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
    snprintf(summary,sizeof(summary),"R2 moved '%.200s' to %.200s.",name,container);
    snprintf(details,sizeof(details),"Object=%.800s; destination=%.800s",name,container);
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
    if (!*old_container || !container_accessible(old_container)) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
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


/* Convert the learned -2..+2 subjective food rating into the shared
 * 0..100 enjoyment scale used by the habit evaluator. Fullness/satiety is
 * deliberately independent; unrated food starts neutral, not pre-liked. */
static int food_preference_enjoyment_score(const char *food)
{
    if (!food || !*food || !reality_db) return 50;
    int score = 50;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT satisfaction_sum * 1.0 / rating_count FROM r2_food_preferences "
        "WHERE food_name=? COLLATE NOCASE AND rating_count>0",
        -1, &st, NULL) == SQLITE_OK) {
        bind_text(st, 1, food);
        if (sqlite3_step(st) == SQLITE_ROW) {
            double average = sqlite3_column_double(st, 0);
            score = (int)lround(50.0 + average * 25.0);
            if (score < 0) score = 0;
            if (score > 100) score = 100;
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    return score;
}

static int reality_eat_internal(const char *food, double fullness, int consume_tracked_item, double energy_override)
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
    if (energy_override >= 0.0) energy_bonus = energy_override;
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
    if (consume_tracked_item && sqlite3_prepare_v2(reality_db,
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
    if (consume_tracked_item && (quantity < 1 || !container_accessible(container))) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }

    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=MAX(0,hunger-?), satisfaction=MIN(150,satisfaction+?), seconds_since_meal=0, energy=MIN(100,energy+?), updated_at=CURRENT_TIMESTAMP WHERE id=1",
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
    if (rc == SQLITE_DONE && consume_tracked_item && quantity > 0) {
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
    if (rc == SQLITE_DONE) {
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
    snprintf(summary, sizeof(summary), "R2 ate %s; satisfaction increased and hunger decreased by the food's %.1f-point fullness value.", food, fullness);
    snprintf(details, sizeof(details), "Food=%s; modeled satiety increase=%.1f points (maximum 150); modeled hunger reduction=%.1f/100; tracked object consumed=%s; these are simulated need values, not measured biological quantities.",
             food, fullness, fullness, consumed_tracked_item ? "yes" : "no");
    bridge_event("food_consumed", summary, details, 1, 0);
    /* A real eating choice feeds the shared habit evaluator using R2's
       accumulated subjective ratings for this food; unrated food is neutral. */
    int learned_enjoyment = food_preference_enjoyment_score(food);
    char habit_details[1400];
    snprintf(habit_details, sizeof(habit_details),
             "%s; learned food enjoyment=%d/100 (0-100 mapping from -2..+2 preference ratings).",
             details, learned_enjoyment);
    (void)r2_addiction_record_choice(food, "food", learned_enjoyment,
                                    "eating", habit_details);
    (void)r2_reward_apply(food, "food_consumed", 2,
        "Food consumption completed and modeled hunger/satiety state was updated.", 0);
    return 0;
}


int r2_reality_eat(const char *food, double fullness)
{
    return reality_eat_internal(food, fullness, 1, -1.0);
}

char *r2_reality_food_context(const char *food)
{
    if (!food || !*food || !r2_reality_is_initialized()) return NULL;
    char ingredients[1024] = {0}, taste[1024] = {0};
    double fullness = -1.0, energy = 0.0;
    (void)food_metric(food, &fullness, &energy);
    food_attributes(food, ingredients, sizeof(ingredients), taste, sizeof(taste));
    char fullness_text[64] = {0};
    if (fullness < 0) snprintf(fullness_text, sizeof(fullness_text), "not configured");
    else snprintf(fullness_text, sizeof(fullness_text), "%.1f/100", fullness);
    char *preferences = query_text(
        "SELECT ingredient,printf('average enjoyment rating %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_ingredient_preferences WHERE rating_count>0 ORDER BY satisfaction_sum*1.0/rating_count DESC", NULL);
    char *history = query_text(
        "SELECT food_name,printf('enjoyment rating %+d/2',satisfaction),notes FROM r2_food_experiences WHERE food_name=? COLLATE NOCASE AND satisfaction IS NOT NULL ORDER BY eaten_at DESC LIMIT 10", food);
    char *food_pref = query_text(
        "SELECT food_name,printf('average enjoyment rating %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_preferences WHERE food_name=? COLLATE NOCASE AND rating_count>0", food);
    if (!preferences || !history || !food_pref) {
        free(preferences); free(history); free(food_pref); return NULL;
    }
    size_t n = strlen(food)+strlen(ingredients)+strlen(taste)+strlen(preferences)+strlen(history)+strlen(food_pref)+1024;
    char *out = malloc(n);
    if (out) snprintf(out,n,
        "FOOD EXPERIENCE CONTEXT (learned subjective preferences; not hard-coded):\n"
        "Food: %s\nConfigured fullness: %s\nConfigured energy bonus: %.1f\nIngredients: %s\nSensory description: %s\n"
        "Past experiences with this food:\n%sFood preference summary:\n%sIngredient preference summaries:\n%s"
        "Food enjoyment is a subjective modeled rating from -2 (strong dislike) to +2 (strong enjoyment); 0 means neutral/uncertain. "
        "Fullness and satisfaction are independent. Ingredient summaries are learned only from rated eating experiences.",
        food, fullness_text, energy,
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
    snprintf(summary,sizeof(summary),"R2's modeled enjoyment rating for %s was %+d/2.",food,satisfaction);
    snprintf(details,sizeof(details),"Food=%s; ingredients=%s; sensory description=%s; enjoyment_rating=%d/2; reason=%s. This is a learned subjective simulation, not an externally verified reaction.",
        food,*ingredients?ingredients:"not specified",*taste?taste:"not specified",satisfaction,notes?notes:"not supplied");
    bridge_event("food_preference_learned",summary,details,1,0);
    /* Synchronize the habit evaluator with the accumulated average rating,
       so one rating informs enjoyment but does not erase earlier experience. */
    int learned_enjoyment = food_preference_enjoyment_score(food);
    (void)r2_addiction_rate_enjoyment(food, "food", learned_enjoyment,
                                      "food_feedback", notes);
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

char *r2_reality_money_context(void)
{
    if (!r2_reality_is_initialized()) return NULL;
    sqlite3_int64 cash = 0, bank = 0;
    pthread_mutex_lock(&reality_lock);int ok=money_read_locked(&cash,&bank)==0;pthread_mutex_unlock(&reality_lock);if(!ok)return NULL;
    money_mirror_write(cash,bank);char out[512];snprintf(out,sizeof(out),"MONEY ACCOUNT: carried cash=$%.2f; bank/piggybank=$%.2f; total=$%.2f. No funds are created automatically.",cash/100.0,bank/100.0,(cash+bank)/100.0);return strdup(out);
}
static int money_transfer(double amount,int deposit)
{
    if(!r2_reality_is_initialized()||!isfinite(amount)||amount<=0||amount>1000000000.0)return -1;
    sqlite3_int64 cents=(sqlite3_int64)llround(amount*100.0),cash=0,bank=0;if(cents<=0)return -1;
    pthread_mutex_lock(&reality_lock);int rc=money_read_locked(&cash,&bank);
    if(rc==0&&((deposit&&cash<cents)||(!deposit&&bank<cents)))rc=-1;
    if(rc==0){sqlite3_stmt *st=NULL;const char *sql=deposit?"UPDATE r2_money_account SET cash_cents=cash_cents-?,bank_cents=bank_cents+?,updated_at=CURRENT_TIMESTAMP WHERE id=1":"UPDATE r2_money_account SET cash_cents=cash_cents+?,bank_cents=bank_cents-?,updated_at=CURRENT_TIMESTAMP WHERE id=1";
        if(sqlite3_prepare_v2(reality_db,sql,-1,&st,NULL)!=SQLITE_OK)rc=-1;else{sqlite3_bind_int64(st,1,cents);sqlite3_bind_int64(st,2,cents);rc=sqlite3_step(st)==SQLITE_DONE?0:-1;}if(st)sqlite3_finalize(st);}
    if (rc == 0) (void)money_read_locked(&cash, &bank);
    pthread_mutex_unlock(&reality_lock);
    if (rc == 0) money_mirror_write(cash, bank);
    return rc;
}
int r2_reality_money_receive(double amount)
{
    if (!r2_reality_is_initialized() || !isfinite(amount) || amount <= 0.0 || amount > 1000000000.0) return -1;
    sqlite3_int64 cents = (sqlite3_int64)llround(amount * 100.0), cash = 0, bank = 0;
    if (cents <= 0) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db, "UPDATE r2_money_account SET cash_cents=cash_cents+?,updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_int64(st, 1, cents); rc = sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) { (void)money_read_locked(&cash, &bank); rc = 0; } else rc = -1;
    pthread_mutex_unlock(&reality_lock);
    if (rc == 0) money_mirror_write(cash, bank);
    return rc;
}
int r2_reality_money_deposit(double amount){return money_transfer(amount,1);}
int r2_reality_money_withdraw(double amount){return money_transfer(amount,0);}
int r2_reality_buy_item(const char *name,const char *description,double price,const char *container)
{
    if(!name||!*name||!isfinite(price)||price<=0||price>1000000000.0||!r2_reality_is_initialized())return -1;
    sqlite3_int64 cents=(sqlite3_int64)llround(price*100.0),cash=0,bank=0,pc=0,pb=0;if(cents<=0)return -1;
    pthread_mutex_lock(&reality_lock);int rc=money_read_locked(&cash,&bank);if(rc==0&&cash+bank<cents)rc=-1;
    if(rc==0){pc=cash<cents?cash:cents;pb=cents-pc;sqlite3_stmt *st=NULL;
        if(sqlite3_prepare_v2(reality_db,"UPDATE r2_money_account SET cash_cents=cash_cents-?,bank_cents=bank_cents-?,updated_at=CURRENT_TIMESTAMP WHERE id=1",-1,&st,NULL)!=SQLITE_OK)rc=-1;
        else{sqlite3_bind_int64(st,1,pc);sqlite3_bind_int64(st,2,pb);rc=sqlite3_step(st)==SQLITE_DONE?0:-1;}if(st)sqlite3_finalize(st);if(rc==0){cash-=pc;bank-=pb;}}
    pthread_mutex_unlock(&reality_lock);if(rc!=0)return -1;
    if(r2_reality_add_item(name,description?description:"Purchased item",container&&*container?container:"pockets",1)!=0){
        pthread_mutex_lock(&reality_lock);sqlite3_stmt *st=NULL;
        if(sqlite3_prepare_v2(reality_db,"UPDATE r2_money_account SET cash_cents=cash_cents+?,bank_cents=bank_cents+?,updated_at=CURRENT_TIMESTAMP WHERE id=1",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int64(st,1,pc);sqlite3_bind_int64(st,2,pb);(void)sqlite3_step(st);}if(st)sqlite3_finalize(st);
        (void)money_read_locked(&cash,&bank);pthread_mutex_unlock(&reality_lock);money_mirror_write(cash,bank);return -1;}
    money_mirror_write(cash,bank);char summary[512],details[1024];snprintf(summary,sizeof(summary),"R2 purchased %s for $%.2f.",name,cents/100.0);
    snprintf(details,sizeof(details),"Item=%s; price=$%.2f; destination=%s; cash paid first, then bank.",name,cents/100.0,container&&*container?container:"pockets");bridge_event("purchase",summary,details,1,1);return 0;
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


/* TV is a persistent device model, not the video player itself. Input 1 is
 * the built-in VCR; external AV inputs and RF signals exist only when a
 * device has been explicitly connected. An empty tuned channel stays empty:
 * no snow-show or synthetic signal is generated here. */
char *r2_reality_tv_status(void)
{
    if (!reality_db || !reality_ready) return strdup("TV state is unavailable: Reality is not initialized.\n");
    char *out = calloc(1, 8192);
    if (!out) return NULL;
    size_t len = 0;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int power = 0, value = 1;
    char kind[16] = "input";
    if (sqlite3_prepare_v2(reality_db, "SELECT power,source_kind,source_value FROM r2_tv_state WHERE id=1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        power = sqlite3_column_int(st, 0);
        const unsigned char *k = sqlite3_column_text(st, 1);
        if (k) snprintf(kind, sizeof(kind), "%s", (const char *)k);
        value = sqlite3_column_int(st, 2);
    }
    if (st) sqlite3_finalize(st);
    len += (size_t)snprintf(out + len, 8192 - len,
        "CRT TV: %s\nSelected source: %s %d\n",
        power ? "ON" : "OFF",
        !strcmp(kind, "vcr") ? "built-in VCR/input" :
        !strcmp(kind, "rf") ? "RF channel" : (value == 1 ? "built-in VCR/input" : "AV input"), value);
    int signal = 0;
    if (!strcmp(kind, "rf") || (!strcmp(kind, "input") && value > 1)) {
        if (sqlite3_prepare_v2(reality_db,
            "SELECT name FROM r2_tv_devices WHERE connected=1 AND connection_kind=? AND port=? LIMIT 1",
            -1, &st, NULL) == SQLITE_OK) {
            bind_text(st, 1, !strcmp(kind, "rf") ? "rf" : "input");
            sqlite3_bind_int(st, 2, value);
            if (sqlite3_step(st) == SQLITE_ROW) signal = 1;
        }
        if (st) sqlite3_finalize(st);
    } else if (!strcmp(kind, "vcr") || (!strcmp(kind, "input") && value == 1)) {
        if (sqlite3_prepare_v2(reality_db,
            "SELECT cassette_path FROM r2_tv_vcr_state WHERE id=1",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            signal = sqlite3_column_type(st, 0) != SQLITE_NULL;
        if (st) sqlite3_finalize(st);
    }
    len += (size_t)snprintf(out + len, 8192 - len, "Selected source signal: %s\n",
                            signal ? "available" : "NO SIGNAL (nothing is connected/transmitting)");
    if (sqlite3_prepare_v2(reality_db,
        "SELECT name,connection_kind,port FROM r2_tv_devices WHERE connected=1 ORDER BY connection_kind,port,name",
        -1, &st, NULL) == SQLITE_OK) {
        int count = 0;
        while (sqlite3_step(st) == SQLITE_ROW && len < 7600) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *dkind = (const char *)sqlite3_column_text(st, 1);
            int port = sqlite3_column_int(st, 2);
            if (!count) len += (size_t)snprintf(out + len, 8192 - len, "Detected external devices:\n");
            len += (size_t)snprintf(out + len, 8192 - len, "  %s — %s %d\n",
                                    name ? name : "unnamed device",
                                    dkind && !strcmp(dkind, "rf") ? "RF channel" : "AV input",
                                    port);
            count++;
        }
        if (!count) len += (size_t)snprintf(out + len, 8192 - len, "Detected external devices: none\n");
    }
    if (st) sqlite3_finalize(st);
    if (sqlite3_prepare_v2(reality_db,
        "SELECT s.cassette_path,s.transport,t.position_seconds "
        "FROM r2_tv_vcr_state s LEFT JOIN r2_tv_vcr_tapes t ON t.media_path=s.cassette_path WHERE s.id=1",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *tape = (const char *)sqlite3_column_text(st, 0);
        const char *transport = (const char *)sqlite3_column_text(st, 1);
        if (tape) {
            len += (size_t)snprintf(out + len, 8192 - len,
                "Built-in VCR: tape inserted (%s), saved position %.1f seconds\n",
                transport ? transport : "stop", sqlite3_column_double(st, 2));
            len += (size_t)snprintf(out + len, 8192 - len, "Tape path: %s\n", tape);
        } else {
            len += (size_t)snprintf(out + len, 8192 - len, "Built-in VCR: empty\n");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    return out;
}

int r2_reality_tv_power(int on)
{
    if (!reality_db || !reality_ready || (on != 0 && on != 1)) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_tv_state SET power=?,updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_int(st, 1, on); rc = sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    bridge_event("tv_power", on ? "R2's CRT television was turned on." : "R2's CRT television was turned off.",
                 "TV power is independent of whether R2 is paying visual attention; no Eyes state is changed by this operation.",
                 1, 0);
    return 0;
}

int r2_reality_tv_select_input(int input)
{
    if (!reality_db || !reality_ready || input < 1 || input > 4) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_tv_state SET source_kind='input',source_value=?,updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_int(st, 1, input); rc = sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    char detail[160];
    snprintf(detail, sizeof(detail), "Selected AV input %d. Input 1 is the built-in VCR; external inputs have signal only when a device is connected.", input);
    bridge_event("tv_source_selected", "R2's CRT television selected an AV input.", detail, 1, 0);
    return 0;
}

int r2_reality_tv_tune_rf(int channel)
{
    if (!reality_db || !reality_ready || channel < 2 || channel > 13) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_tv_state SET source_kind='rf',source_value=?,updated_at=CURRENT_TIMESTAMP WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_int(st, 1, channel); rc = sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    char detail[160];
    snprintf(detail, sizeof(detail), "Tuned RF channel %d. A signal is available only if a connected device is transmitting on that channel; no static or snow-show signal is generated.", channel);
    bridge_event("tv_rf_tuned", "R2's CRT television was tuned to an RF channel.", detail, 1, 0);
    return 0;
}

int r2_reality_tv_connect(const char *name, const char *kind, int port)
{
    if (!reality_db || !reality_ready || !name || !*name || strlen(name) > 120 || !kind) return -1;
    int rf = !strcasecmp(kind, "rf");
    int input = !strcasecmp(kind, "input") || !strcasecmp(kind, "av");
    if ((!rf && !input) || (rf && (port < 2 || port > 13)) ||
        (input && (port < 2 || port > 4))) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_tv_devices(name,connection_kind,port,connected,updated_at) VALUES(?,?,?,1,CURRENT_TIMESTAMP) "
        "ON CONFLICT(name) DO UPDATE SET connection_kind=excluded.connection_kind,port=excluded.port,connected=1,updated_at=CURRENT_TIMESTAMP",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name); bind_text(st, 2, rf ? "rf" : "input"); sqlite3_bind_int(st, 3, port);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    char detail[256];
    snprintf(detail, sizeof(detail), "%s is connected to %s %d; this is now a detected source. No gameplay is inferred from connection alone.",
             name, rf ? "RF channel" : "AV input", port);
    bridge_event("tv_device_connected", "An external device was connected to R2's CRT television.", detail, 1, 0);
    return 0;
}

int r2_reality_tv_disconnect(const char *name)
{
    if (!reality_db || !reality_ready || !name || !*name) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_tv_devices SET connected=0,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE AND connected=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) { bind_text(st, 1, name); rc = sqlite3_step(st); }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(reality_db) : 0;
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE || changed == 0) return -1;
    char detail[192];
    snprintf(detail, sizeof(detail), "%s was disconnected; any tuned source now has no signal unless another device supplies it.", name);
    bridge_event("tv_device_disconnected", "An external device was disconnected from R2's CRT television.", detail, 1, 0);
    return 0;
}

/* Path-keyed tape identities preserve position through eject/reinsert and restarts.
 * Reality owns transport state; the GUI is only the video renderer. */
static int tv_vcr_media_path_valid(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) > 1024 ||
        strchr(path, '\n') || strchr(path, '\r')) return 0;
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    static const char *extensions[] = {
        ".avi", ".mkv", ".mp4", ".m4v", ".mov", ".mpeg", ".mpg",
        ".wmv", ".webm", ".ogv", ".flv", ".ts", ".vob", ".3gp",
        ".asf", ".m2ts", ".mts"
    };
    int supported = 0;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        if (!strcasecmp(dot, extensions[i])) { supported = 1; break; }
    if (!supported) return 0;
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, R_OK) == 0;
}

int r2_reality_tv_vcr_insert(const char *media_path)
{
    if (!reality_db || !reality_ready || !tv_vcr_media_path_valid(media_path)) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT OR IGNORE INTO r2_tv_vcr_tapes(media_path,position_seconds) VALUES(?,0)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) { bind_text(st, 1, media_path); rc = sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_tv_vcr_state SET cassette_path=?,transport='stop',updated_at=CURRENT_TIMESTAMP WHERE id=1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) { bind_text(st, 1, media_path); rc = sqlite3_step(st); }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE) return -1;
    char detail[1200];
    snprintf(detail, sizeof(detail), "Inserted VCR tape: %s. Its saved playback position is retained.", media_path);
    bridge_event("vcr_tape_inserted", "A tape was inserted into R2's built-in VCR.", detail, 1, 0);
    return 0;
}

int r2_reality_tv_vcr_transport(const char *action)
{
    if (!reality_db || !reality_ready || !action) return -1;
    int eject = !strcasecmp(action, "eject");
    if (!eject && strcasecmp(action, "play") && strcasecmp(action, "pause") &&
        strcasecmp(action, "stop")) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = SQLITE_ERROR;
    if (eject) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_tv_vcr_state SET cassette_path=NULL,transport='stop',updated_at=CURRENT_TIMESTAMP WHERE id=1 AND cassette_path IS NOT NULL",
            -1, &st, NULL);
    } else if (!strcasecmp(action, "play")) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_tv_vcr_state SET transport='play',updated_at=CURRENT_TIMESTAMP WHERE id=1 AND cassette_path IS NOT NULL",
            -1, &st, NULL);
    } else {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_tv_vcr_state SET transport=?,updated_at=CURRENT_TIMESTAMP WHERE id=1 AND cassette_path IS NOT NULL",
            -1, &st, NULL);
        if (rc == SQLITE_OK) bind_text(st, 1, !strcasecmp(action, "pause") ? "pause" : "stop");
    }
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(reality_db) : 0;
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE || changed == 0) return -1;
    char detail[128];
    snprintf(detail, sizeof(detail), "Built-in VCR transport changed to %s.", eject ? "eject" : action);
    bridge_event(eject ? "vcr_tape_ejected" : "vcr_transport_changed",
                 eject ? "The tape was ejected from R2's built-in VCR." : "R2's built-in VCR transport changed.",
                 detail, 1, 0);
    return 0;
}

int r2_reality_tv_vcr_set_position(double seconds)
{
    if (!reality_db || !reality_ready || !isfinite(seconds) || seconds < 0.0 || seconds > 604800.0) return -1;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_tv_vcr_tapes SET position_seconds=?,updated_at=CURRENT_TIMESTAMP "
        "WHERE media_path=(SELECT cassette_path FROM r2_tv_vcr_state WHERE id=1 AND cassette_path IS NOT NULL)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_double(st, 1, seconds); rc = sqlite3_step(st); }
    int changed = rc == SQLITE_DONE ? sqlite3_changes(reality_db) : 0;
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    return rc == SQLITE_DONE && changed > 0 ? 0 : -1;
}
