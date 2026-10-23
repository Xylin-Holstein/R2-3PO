#define _POSIX_C_SOURCE 200809L
#include "Reality.h"

#include <sqlite3.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef R2_ROOT
#define R2_ROOT "/tmp/r2-needs-smoke"
#endif
#ifndef R2_HOME
#define R2_HOME "/tmp/r2-needs-smoke/R2"
#endif

static void fail(const char *message)
{
    fprintf(stderr, "reality needs smoke failed: %s\n", message);
    r2_reality_shutdown();
    exit(1);
}

static void read_needs(double *hunger, double *sleepiness, double *energy,
                       double *satisfaction, double *since_meal)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database");
    if (sqlite3_prepare_v2(db,
        "SELECT hunger,sleepiness,energy,satisfaction,seconds_since_meal "
        "FROM r2_reality_self WHERE id=1", -1, &st, NULL) != SQLITE_OK)
        fail("could not prepare needs query");
    if (sqlite3_step(st) != SQLITE_ROW) fail("needs row missing");
    *hunger = sqlite3_column_double(st, 0);
    *sleepiness = sqlite3_column_double(st, 1);
    *energy = sqlite3_column_double(st, 2);
    *satisfaction = sqlite3_column_double(st, 3);
    *since_meal = sqlite3_column_double(st, 4);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static sqlite3_int64 read_world_elapsed(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database for world time");
    if (sqlite3_prepare_v2(db,
        "SELECT CAST(value AS INTEGER) FROM r2_reality_meta WHERE key='world_elapsed_seconds'",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare world-time query");
    if (sqlite3_step(st) != SQLITE_ROW) fail("world-time row missing");
    sqlite3_int64 value = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

static int starvation_flag_is_yes(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database for starvation flag");
    if (sqlite3_prepare_v2(db,
        "SELECT value FROM r2_reality_self_facts WHERE key='starvation_72h_logged'",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare starvation-flag query");
    int yes = sqlite3_step(st) == SQLITE_ROW &&
              strcmp((const char *)sqlite3_column_text(st, 0), "yes") == 0;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return yes;
}

static void seed_eight_day_gap(const char *path)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database for long-gap test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=100,sleepiness=0,energy=100,"
        "satisfaction=100,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare long-gap state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) - 8 * 86400);
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not seed long-gap state");
    sqlite3_finalize(st);
    if (sqlite3_exec(db,
        "INSERT INTO r2_reality_self_facts(key,value,evidence) "
        "VALUES('starvation_72h_logged','no','Long-gap regression reset') "
        "ON CONFLICT(key) DO UPDATE SET value='no',evidence='Long-gap regression reset',updated_at=CURRENT_TIMESTAMP",
        NULL, NULL, NULL) != SQLITE_OK) fail("could not reset starvation marker for long-gap test");
    sqlite3_close(db);
}

static void seed_legacy_reality_owned_tables(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not create legacy Reality-owned database fixture");
    const char *legacy =
        "CREATE TABLE r2_reality_item_memory("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,item_name TEXT NOT NULL,"
        "description TEXT,exact_quantity INTEGER,approximate_quantity INTEGER);"
        "INSERT INTO r2_reality_item_memory(item_name,description,exact_quantity)"
        " VALUES('native_old_item','Preserve this in-place Reality row',4);"
        "CREATE TABLE r2_food_experiences("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,food_name TEXT NOT NULL,"
        "ingredients TEXT,fullness REAL NOT NULL,satisfaction INTEGER,notes TEXT,"
        "eaten_at INTEGER NOT NULL,rated_at INTEGER);"
        "INSERT INTO r2_food_experiences(food_name,fullness,eaten_at)"
        " VALUES('native_old_meal',22,123456);"
        "CREATE TABLE r2_reality_self_facts(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO r2_reality_self_facts(key,value) VALUES('native_fact','keep this fact');"
        "CREATE TABLE r2_money_account(id INTEGER PRIMARY KEY,cash_cents INTEGER,bank_cents INTEGER);"
        "INSERT INTO r2_money_account(id,cash_cents,bank_cents) VALUES(1,1234,5678);"
        "CREATE TABLE r2_tv_state(id INTEGER PRIMARY KEY,power INTEGER,source_kind TEXT,source_value INTEGER);"
        "INSERT INTO r2_tv_state(id,power,source_kind,source_value) VALUES(1,1,'input',2);"
        "CREATE TABLE r2_tv_devices(name TEXT PRIMARY KEY,connection_kind TEXT,port INTEGER);"
        "INSERT INTO r2_tv_devices(name,connection_kind,port) VALUES"
        "('native_old_device','input',3),('native_duplicate_device','input',3);"
        "CREATE TABLE r2_tv_vcr_tapes(media_path TEXT PRIMARY KEY);"
        "INSERT INTO r2_tv_vcr_tapes(media_path) VALUES('native_old_tape.mkv');"
        "CREATE TABLE r2_tv_vcr_state(id INTEGER PRIMARY KEY,cassette_path TEXT,transport TEXT);"
        "INSERT INTO r2_tv_vcr_state(id,cassette_path,transport) VALUES(1,'legacy_tape.mkv','pause');"
        "CREATE TABLE r2_reality_containers(name TEXT PRIMARY KEY,description TEXT,parent TEXT);"
        "INSERT INTO r2_reality_containers(name,description,parent)"
        " VALUES('native_old_container','Old in-place container','room');"
        "CREATE TABLE r2_reality_objects("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE,"
        "description TEXT,quantity INTEGER,container TEXT);"
        "INSERT INTO r2_reality_objects(name,description,quantity,container)"
        " VALUES('native_old_object','Old in-place object',3,'native_old_container');"
        "CREATE TABLE r2_reality_ticks("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,previous_tick INTEGER,current_tick INTEGER);"
        "INSERT INTO r2_reality_ticks(previous_tick,current_tick) VALUES(7,12);"
        "CREATE TABLE r2_food_preferences("
        "food_name TEXT PRIMARY KEY,satisfaction_sum REAL,rating_count INTEGER);"
        "INSERT INTO r2_food_preferences(food_name,satisfaction_sum,rating_count)"
        " VALUES('native_old_meal',2.5,1);"
        "CREATE TABLE r2_food_ingredient_preferences("
        "ingredient TEXT PRIMARY KEY,satisfaction_sum REAL,rating_count INTEGER);"
        "INSERT INTO r2_food_ingredient_preferences(ingredient,satisfaction_sum,rating_count)"
        " VALUES('native_old_ingredient',1.5,1);";
    char *error = NULL;
    int rc = sqlite3_exec(db, legacy, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "reality needs smoke failed: could not seed legacy Reality DB: %s\n",
                error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        sqlite3_close(db);
        exit(1);
    }
    sqlite3_close(db);
}

static void seed_legacy_fridge_schema(void)
{
    if (mkdir(R2_ROOT, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "reality needs smoke failed: could not create test root\n");
        exit(1);
    }
    if (mkdir(R2_HOME, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "reality needs smoke failed: could not create test data directory\n");
        exit(1);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "reality needs smoke failed: could not create legacy fridge fixture\n");
        if (db) sqlite3_close(db);
        exit(1);
    }
    const char *legacy_schema =
        "CREATE TABLE r2_fridge_items("
        "name TEXT PRIMARY KEY COLLATE NOCASE,"
        "description TEXT,"
        "quantity INTEGER NOT NULL CHECK(quantity>0));"
        "INSERT INTO r2_fridge_items(name,description,quantity) "
        "VALUES('legacy_unknown','Legacy stock without nutrition metadata',2);";
    char *error = NULL;
    int rc = sqlite3_exec(db, legacy_schema, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "reality needs smoke failed: could not seed legacy fridge schema: %s\n",
                error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        sqlite3_close(db);
        exit(1);
    }
    sqlite3_close(db);
}

static void verify_legacy_fridge_migration(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not open migrated legacy fridge");
    if (sqlite3_prepare_v2(db,
        "SELECT quantity,fullness,energy FROM r2_fridge_items WHERE name='legacy_unknown'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW)
        fail("legacy fridge stock disappeared during schema migration");
    int quantity = sqlite3_column_int(st, 0);
    double fullness = sqlite3_column_double(st, 1);
    double energy = sqlite3_column_double(st, 2);
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (quantity != 2)
        fail("legacy fridge quantity must be preserved during migration");
    if (fullness != 0.0 || energy != 0.0)
        fail("legacy stock without nutrition metadata must not receive invented fullness or energy");

    if (r2_reality_fridge_eat("legacy_unknown", 100.0) == 0)
        fail("caller-supplied fullness must not make unknown legacy stock edible");
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not reopen migrated fridge after rejected meal");
    if (sqlite3_prepare_v2(db,
        "SELECT quantity,fullness,energy FROM r2_fridge_items WHERE name='legacy_unknown'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW)
        fail("rejected meal must preserve legacy stock");
    quantity = sqlite3_column_int(st, 0);
    fullness = sqlite3_column_double(st, 1);
    energy = sqlite3_column_double(st, 2);
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (quantity != 2 || fullness != 0.0 || energy != 0.0)
        fail("rejected meal changed unknown legacy stock or its nutrition metadata");

    if (r2_fridge_take("legacy_unknown") != 0 ||
        r2_fridge_take("legacy_unknown") != 0)
        fail("unknown legacy stock should remain movable even when nutrition is unconfigured");
    if (r2_reality_remove_item("legacy_unknown") != 0)
        fail("could not clean up migrated legacy stock fixture");
}

static void seed_legacy_shared_reality_tables(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_memory.db", R2_HOME);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not create legacy shared-memory database fixture");
    const char *sql =
        "CREATE TABLE r2_reality_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO r2_reality_meta(key,value) VALUES('current_location','home'),('initial_cash_seeded','1');"
        "CREATE TABLE r2_reality_containers(name TEXT PRIMARY KEY,description TEXT,parent TEXT);"
        "INSERT INTO r2_reality_containers(name,description,parent) VALUES('legacy_container','Old container','room');"
        "CREATE TABLE r2_reality_objects(name TEXT PRIMARY KEY,description TEXT,quantity INTEGER,container TEXT);"
        "INSERT INTO r2_reality_objects(name,description,quantity,container) VALUES('legacy_stack','Old stack',2,'legacy_container');"
        "CREATE TABLE r2_reality_self_facts(key TEXT PRIMARY KEY,value TEXT);"
        "INSERT INTO r2_reality_self_facts(key,value) VALUES('legacy_fact','preserve me');"
        "CREATE TABLE r2_reality_ticks(previous_tick INTEGER,current_tick INTEGER);"
        "INSERT INTO r2_reality_ticks(previous_tick,current_tick) VALUES(100,110);"
        "CREATE TABLE r2_reality_item_memory(item_name TEXT,description TEXT,exact_quantity INTEGER,collected_at INTEGER);"
        "INSERT INTO r2_reality_item_memory(item_name,description,exact_quantity,collected_at) "
        "VALUES('legacy_collection','Old item memory',3,CAST(strftime('%s','now') AS INTEGER));"
        "CREATE TABLE r2_food_experiences(food_name TEXT,fullness REAL,eaten_at INTEGER);"
        "INSERT INTO r2_food_experiences(food_name,fullness,eaten_at) VALUES('legacy_meal',25,123456);"
        "CREATE TABLE r2_food_preferences(food_name TEXT PRIMARY KEY,satisfaction_sum REAL,rating_count INTEGER,updated_at INTEGER);"
        "INSERT INTO r2_food_preferences(food_name,satisfaction_sum,rating_count,updated_at) "
        "VALUES('legacy_meal',3.5,2,123456);"
        "CREATE TABLE r2_food_ingredient_preferences(ingredient TEXT PRIMARY KEY);"
        "INSERT INTO r2_food_ingredient_preferences(ingredient) VALUES('legacy_ingredient');"
        "CREATE TABLE r2_food_experience_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO r2_food_experience_meta(key,value) VALUES('legacy_test','preserved');"
        "CREATE TABLE r2_money_account(id INTEGER PRIMARY KEY,cash_cents INTEGER,bank_cents INTEGER);"
        "INSERT INTO r2_money_account(id,cash_cents,bank_cents) VALUES(1,1234,5678);"
        "CREATE TABLE r2_tv_state(id INTEGER PRIMARY KEY,power INTEGER,source_kind TEXT,source_value INTEGER);"
        "INSERT INTO r2_tv_state(id,power,source_kind,source_value) VALUES(1,1,'input',2);"
        "CREATE TABLE r2_tv_devices(name TEXT PRIMARY KEY,connection_kind TEXT,port INTEGER);"
        "INSERT INTO r2_tv_devices(name,connection_kind,port) VALUES('legacy_vcr','input',4);"
        "CREATE TABLE r2_tv_vcr_tapes(media_path TEXT PRIMARY KEY);"
        "INSERT INTO r2_tv_vcr_tapes(media_path) VALUES('legacy_tape.mkv');"
        "CREATE TABLE r2_tv_vcr_state(id INTEGER PRIMARY KEY,cassette_path TEXT,transport TEXT);"
        "INSERT INTO r2_tv_vcr_state(id,cassette_path,transport) VALUES(1,'legacy_tape.mkv','pause');";
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "reality needs smoke failed: could not seed legacy shared DB: %s\n",
                error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        sqlite3_close(db);
        exit(1);
    }
    sqlite3_close(db);
}

static void verify_legacy_shared_reality_migration(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not open destination Reality DB after legacy migration");

    const struct { const char *sql; int expected; const char *message; } counts[] = {
        {"SELECT COUNT(*) FROM r2_reality_item_memory WHERE item_name='native_old_item' AND exact_quantity=4", 1,
         "in-place legacy item memory was not preserved"},
        {"SELECT COUNT(*) FROM r2_food_experiences WHERE food_name='native_old_meal' AND fullness=22 AND energy_bonus=0", 1,
         "in-place legacy food experience was not preserved with neutral energy"},
        {"SELECT COUNT(*) FROM r2_reality_self_facts WHERE key='native_fact' AND value='keep this fact'", 1,
         "in-place legacy Reality fact was not preserved"},
        {"SELECT COUNT(*) FROM r2_money_account WHERE id=1 AND cash_cents=1234 AND bank_cents=5678", 1,
         "in-place legacy wallet balances were not preserved"},
        {"SELECT COUNT(*) FROM r2_tv_state WHERE id=1 AND power=1 AND source_value=2", 1,
         "in-place legacy TV state was not preserved"},
        {"SELECT COUNT(*) FROM r2_tv_devices WHERE name='native_old_device' AND connected=1", 1,
         "in-place legacy TV device was not preserved and upgraded"},
        {"SELECT COUNT(*) FROM r2_tv_devices WHERE name='native_duplicate_device' AND connected=0", 1,
         "legacy TV port collision should preserve the row but deactivate the conflicting connection"},
        {"SELECT COUNT(*) FROM r2_tv_vcr_state WHERE cassette_path='legacy_tape.mkv' AND transport='pause'", 1,
         "in-place legacy VCR state was not preserved"},
        {"SELECT COUNT(*) FROM r2_reality_containers WHERE name='native_old_container' AND kind='container'", 1,
         "in-place legacy container was not preserved and upgraded"},
        {"SELECT COUNT(*) FROM r2_reality_objects WHERE name='native_old_object' AND quantity=3 AND container='native_old_container'", 1,
         "in-place legacy object was not preserved and upgraded"},
        {"SELECT COUNT(*) FROM r2_reality_ticks WHERE previous_tick=7 AND current_tick=12 AND elapsed_seconds=0", 1,
         "in-place legacy world tick was not preserved with a neutral elapsed default"},
        {"SELECT COUNT(*) FROM r2_food_preferences WHERE food_name='native_old_meal' AND satisfaction_sum=2.5 AND rating_count=1", 1,
         "in-place legacy food preference was not preserved"},
        {"SELECT COUNT(*) FROM r2_reality_item_memory WHERE item_name='legacy_collection'", 1,
         "legacy item-collection memory was not migrated"},
        {"SELECT COUNT(*) FROM r2_food_experiences WHERE food_name='legacy_meal'", 1,
         "legacy food experience was not migrated"},
        {"SELECT COUNT(*) FROM r2_food_ingredient_preferences WHERE ingredient='legacy_ingredient'", 1,
         "legacy ingredient preference was not migrated"},
        {"SELECT COUNT(*) FROM r2_food_experience_meta WHERE key='legacy_test' AND value='preserved'", 1,
         "legacy food experience metadata was not migrated"},
        {"SELECT COUNT(*) FROM r2_reality_self_facts WHERE key='legacy_fact' AND value='preserve me'", 1,
         "legacy self-fact was not migrated"},
        {"SELECT COUNT(*) FROM r2_reality_objects WHERE name='legacy_stack' AND quantity=2 AND container='legacy_container'", 1,
         "legacy object stack/container was not migrated"},
        {"SELECT COUNT(*) FROM r2_reality_containers WHERE name='legacy_container' AND kind='container'", 1,
         "legacy container was not migrated with a safe default kind"},
        {"SELECT COUNT(*) FROM r2_reality_ticks WHERE previous_tick=100 AND current_tick=110 AND elapsed_seconds=10", 1,
         "legacy world tick was not migrated with elapsed-time fallback"},
        {"SELECT COUNT(*) FROM r2_tv_vcr_tapes WHERE media_path='legacy_tape.mkv'", 1,
         "legacy VCR tape was not migrated"},
        {"SELECT COUNT(*) FROM r2_tv_devices WHERE name='legacy_vcr' AND connected=1", 1,
         "legacy TV device was not migrated with its default connection state"},
    };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        if (sqlite3_prepare_v2(db, counts[i].sql, -1, &st, NULL) != SQLITE_OK ||
            sqlite3_step(st) != SQLITE_ROW ||
            sqlite3_column_int(st, 0) != counts[i].expected) {
            if (st) sqlite3_finalize(st);
            sqlite3_close(db);
            fail(counts[i].message);
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    if (sqlite3_prepare_v2(db,
        "SELECT precision,last_decay_at,collected_at FROM r2_reality_item_memory WHERE item_name='legacy_collection'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(st, 0), "exact") != 0 ||
        sqlite3_column_int64(st, 1) != sqlite3_column_int64(st, 2)) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy item memory did not receive backward-compatible precision/decay defaults");
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
        "SELECT fullness,energy_bonus FROM r2_food_experiences WHERE food_name='legacy_meal'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_double(st, 0) != 25.0 || sqlite3_column_double(st, 1) != 0.0) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy food experience was not preserved with neutral missing-energy fallback");
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
        "SELECT satisfaction_sum,rating_count FROM r2_food_preferences WHERE food_name='legacy_meal'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_double(st, 0) != 3.5 || sqlite3_column_int(st, 1) != 2) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy food ratings were not preserved/merged");
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
        "SELECT cash_cents,bank_cents FROM r2_money_account WHERE id=1",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_int64(st, 0) != 1234 || sqlite3_column_int64(st, 1) != 5678) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy cash and piggybank balances were not preserved");
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
        "SELECT power,source_kind,source_value FROM r2_tv_state WHERE id=1",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_int(st, 0) != 1 ||
        strcmp((const char *)sqlite3_column_text(st, 1), "input") != 0 ||
        sqlite3_column_int(st, 2) != 2) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy CRT power/input state was not preserved");
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
        "SELECT cassette_path,transport FROM r2_tv_vcr_state WHERE id=1",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(st, 0), "legacy_tape.mkv") != 0 ||
        strcmp((const char *)sqlite3_column_text(st, 1), "pause") != 0) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy VCR cassette and transport state were not preserved");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    /* Migration is copy-only: old memory/diary data must still be present. */
    snprintf(path, sizeof(path), "%s/r2_memory.db", R2_HOME);
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not reopen original memory DB after migration");
    st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM r2_food_experiences WHERE food_name='legacy_meal'",
        -1, &st, NULL) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_int(st, 0) != 1) {
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        fail("legacy memory database was modified or cleared by Reality migration");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

int main(void)
{
    seed_legacy_fridge_schema();
    seed_legacy_reality_owned_tables();
    seed_legacy_shared_reality_tables();
    if (r2_reality_init() != 0) fail("Reality initialization failed");
    char *initial_status = r2_reality_status();
    if (!initial_status || !strstr(initial_status, "SELF CONTINUITY") ||
        !strstr(initial_status, "Hunger:")) {
        free(initial_status);
        fail("valid authoritative Reality state did not produce needs/status context");
    }
    free(initial_status);
    char *initial_tv_status = r2_reality_tv_status();
    /* This fixture deliberately seeds the legacy TV's powered-on AV-input
       state and inserted VCR tape; migration must preserve both exactly. */
    if (!initial_tv_status || !strstr(initial_tv_status, "CRT TV: ON") ||
        !strstr(initial_tv_status, "Selected source: AV input 2") ||
        !strstr(initial_tv_status, "Built-in VCR: tape inserted (pause)") ||
        !strstr(initial_tv_status, "legacy_tape.mkv")) {
        if (initial_tv_status)
            fprintf(stderr, "Reality TV status fixture output:\n%s\n", initial_tv_status);
        free(initial_tv_status);
        fail("migrated legacy TV power/source/VCR state was not represented in device status");
    }
    free(initial_tv_status);
    verify_legacy_fridge_migration();
    verify_legacy_shared_reality_migration();
    double initial_hunger, initial_sleepiness, initial_energy;
    double initial_satisfaction, initial_since_meal;
    read_needs(&initial_hunger, &initial_sleepiness, &initial_energy,
               &initial_satisfaction, &initial_since_meal);
    if (initial_hunger < 99.9 || initial_hunger > 100.0)
        fail("fresh R2 state should start at full modeled satiety after the one-time semantics migration");
    if (r2_reality_add_item("invalid_quantity", "Invalid quantity test", "pockets", 0) == 0)
        fail("Reality API must reject non-positive quantities rather than converting them to one");
    if (r2_reality_add_item("overflow_stack", "Quantity boundary test", "pockets", INT_MAX) != 0)
        fail("Reality API should accept a representable maximum stack");
    if (r2_reality_add_item("overflow_stack", "Quantity boundary test", "pockets", 1) == 0)
        fail("Reality API must reject a stack addition that overflows its int mirror quantity");
    sqlite3 *overflow_db = NULL;
    if (sqlite3_open(R2_HOME "/r2_reality.db", &overflow_db) != SQLITE_OK)
        fail("could not query quantity overflow regression");
    sqlite3_stmt *overflow_st = NULL;
    if (sqlite3_prepare_v2(overflow_db,
        "SELECT quantity FROM r2_reality_objects WHERE name='overflow_stack'",
        -1, &overflow_st, NULL) != SQLITE_OK ||
        sqlite3_step(overflow_st) != SQLITE_ROW ||
        sqlite3_column_int64(overflow_st, 0) != INT_MAX)
        fail("overflowing stack addition changed authoritative quantity");
    sqlite3_finalize(overflow_st);
    sqlite3_close(overflow_db);
    if (r2_reality_remove_item("overflow_stack") != 0)
        fail("could not clean up quantity overflow fixture");

    /* Adding another unit must not silently move an existing stack from the
       shelf into pockets. Moving the stack is a separate, explicit operation. */
    if (r2_reality_add_item("cross_container_stack", "Shelf stack", "shelf", 2) != 0)
        fail("could not create cross-container stack fixture");
    if (r2_reality_add_item("cross_container_stack", "Pocket stack", "pockets", 1) == 0)
        fail("adding an item in another container must not silently relocate the existing stack");
    sqlite3 *container_db = NULL;
    sqlite3_stmt *container_st = NULL;
    if (sqlite3_open(R2_HOME "/r2_reality.db", &container_db) != SQLITE_OK)
        fail("could not query cross-container stack regression");
    if (sqlite3_prepare_v2(container_db,
        "SELECT container,quantity FROM r2_reality_objects WHERE name='cross_container_stack'",
        -1, &container_st, NULL) != SQLITE_OK ||
        sqlite3_step(container_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(container_st, 0), "shelf") != 0 ||
        sqlite3_column_int(container_st, 1) != 2)
        fail("rejected cross-container add changed the existing shelf stack");
    sqlite3_finalize(container_st);
    sqlite3_close(container_db);
    if (r2_reality_move_item("cross_container_stack", "pockets") != 0)
        fail("explicit stack move from shelf to pockets should succeed");
    if (sqlite3_open(R2_HOME "/r2_reality.db", &container_db) != SQLITE_OK)
        fail("could not verify explicit stack move");
    container_st = NULL;
    if (sqlite3_prepare_v2(container_db,
        "SELECT container,quantity FROM r2_reality_objects WHERE name='cross_container_stack'",
        -1, &container_st, NULL) != SQLITE_OK ||
        sqlite3_step(container_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(container_st, 0), "pockets") != 0 ||
        sqlite3_column_int(container_st, 1) != 2)
        fail("explicit stack move did not preserve quantity while changing containers");
    sqlite3_finalize(container_st);
    sqlite3_close(container_db);
    if (r2_reality_remove_item("cross_container_stack") != 0)
        fail("could not clean up cross-container stack fixture");

    /* Force a secondary persistence failure and verify the primary object,
       its container, and its collection memory are rolled back together. */
    char atomic_path[1024];
    snprintf(atomic_path, sizeof(atomic_path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *atomic_db = NULL;
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not open Reality DB for atomic-add regression");
    if (sqlite3_exec(atomic_db,
        "CREATE TRIGGER fail_atomic_item_memory BEFORE INSERT ON r2_reality_item_memory "
        "WHEN NEW.item_name='atomic_failure' BEGIN SELECT RAISE(ABORT,'forced regression failure'); END;",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not install atomic-add failure trigger");
    sqlite3_close(atomic_db);
    if (r2_reality_add_item("atomic_failure", "Must roll back", "atomic_test_container", 1) == 0)
        fail("item addition must fail when its collection memory cannot be persisted");
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not reopen Reality DB after atomic-add failure");
    sqlite3_stmt *atomic_st = NULL;
    const char *atomic_queries[] = {
        "SELECT COUNT(*) FROM r2_reality_objects WHERE name='atomic_failure'",
        "SELECT COUNT(*) FROM r2_reality_containers WHERE name='atomic_test_container'",
        "SELECT COUNT(*) FROM r2_reality_item_memory WHERE item_name='atomic_failure'"
    };
    for (size_t i = 0; i < sizeof(atomic_queries) / sizeof(atomic_queries[0]); ++i) {
        if (sqlite3_prepare_v2(atomic_db, atomic_queries[i], -1, &atomic_st, NULL) != SQLITE_OK ||
            sqlite3_step(atomic_st) != SQLITE_ROW || sqlite3_column_int(atomic_st, 0) != 0)
            fail("failed item addition left a partial world/container/memory record");
        sqlite3_finalize(atomic_st);
        atomic_st = NULL;
    }
    if (sqlite3_exec(atomic_db, "DROP TRIGGER fail_atomic_item_memory", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not remove atomic-add regression trigger");
    sqlite3_close(atomic_db);

    if (r2_reality_add_item("move_failure", "Must stay in pockets", "pockets", 1) != 0)
        fail("could not create item for atomic-move regression");
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not open Reality DB for atomic-move regression");
    if (sqlite3_exec(atomic_db,
        "CREATE TRIGGER fail_atomic_move BEFORE UPDATE OF container ON r2_reality_objects "
        "WHEN NEW.name='move_failure' BEGIN SELECT RAISE(ABORT,'forced move regression failure'); END;",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not install atomic-move failure trigger");
    sqlite3_close(atomic_db);
    if (r2_reality_move_item("move_failure", "atomic_move_container") == 0)
        fail("move must fail when its object update fails");
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not reopen Reality DB after atomic-move failure");
    atomic_st = NULL;
    if (sqlite3_prepare_v2(atomic_db,
        "SELECT container FROM r2_reality_objects WHERE name='move_failure'",
        -1, &atomic_st, NULL) != SQLITE_OK ||
        sqlite3_step(atomic_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(atomic_st, 0), "pockets") != 0)
        fail("failed move changed the object's authoritative container");
    sqlite3_finalize(atomic_st);
    atomic_st = NULL;
    if (sqlite3_prepare_v2(atomic_db,
        "SELECT COUNT(*) FROM r2_reality_containers WHERE name='atomic_move_container'",
        -1, &atomic_st, NULL) != SQLITE_OK ||
        sqlite3_step(atomic_st) != SQLITE_ROW || sqlite3_column_int(atomic_st, 0) != 0)
        fail("failed move left behind a phantom destination container");
    sqlite3_finalize(atomic_st);
    if (sqlite3_exec(atomic_db, "DROP TRIGGER fail_atomic_move", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not remove atomic-move regression trigger");
    sqlite3_close(atomic_db);

    if (r2_reality_add_item("fridge_move_failure", "World copy", "pockets", 1) != 0)
        fail("could not create world item for fridge rollback regression");
    if (r2_fridge_add_item("fridge_move_failure", "Original fridge copy", 2,
                           12.0, 1.0, "old ingredient", "old taste") != 0)
        fail("could not seed existing fridge stock for rollback regression");
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not open Reality DB for fridge rollback regression");
    if (sqlite3_exec(atomic_db,
        "CREATE TRIGGER fail_fridge_move BEFORE DELETE ON r2_reality_objects "
        "WHEN OLD.name='fridge_move_failure' BEGIN SELECT RAISE(ABORT,'forced fridge move failure'); END;",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not install fridge move failure trigger");
    sqlite3_close(atomic_db);
    if (r2_reality_move_item("fridge_move_failure", "fridge") == 0)
        fail("fridge move must fail when source-object deletion fails");
    if (sqlite3_open(atomic_path, &atomic_db) != SQLITE_OK)
        fail("could not reopen Reality DB after fridge move failure");
    atomic_st = NULL;
    if (sqlite3_prepare_v2(atomic_db,
        "SELECT container,quantity FROM r2_reality_objects WHERE name='fridge_move_failure'",
        -1, &atomic_st, NULL) != SQLITE_OK ||
        sqlite3_step(atomic_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(atomic_st, 0), "pockets") != 0 ||
        sqlite3_column_int(atomic_st, 1) != 1)
        fail("failed fridge move changed or removed the source object");
    sqlite3_finalize(atomic_st);
    if (sqlite3_exec(atomic_db, "DROP TRIGGER fail_fridge_move", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not remove fridge move failure trigger");
    sqlite3_close(atomic_db);
    char *fridge_after_rollback = r2_fridge_context();
    if (!fridge_after_rollback ||
        !strstr(fridge_after_rollback, "Original fridge copy") ||
        !strstr(fridge_after_rollback, "quantity=2") ||
        !strstr(fridge_after_rollback, "fullness=12.0/100") ||
        !strstr(fridge_after_rollback, "energy=1.0")) {
        free(fridge_after_rollback);
        fail("failed fridge move did not restore the original stock metadata and quantity");
    }
    free(fridge_after_rollback);
    /* Remove the deliberate rollback fixture so later autonomous-feeding
       assertions exercise only their own explicitly seeded food stock. */
    if (r2_reality_remove_item("fridge_move_failure") != 0)
        fail("could not remove world-side fridge rollback fixture");
    if (r2_fridge_take("fridge_move_failure") != 0 ||
        r2_fridge_take("fridge_move_failure") != 0)
        fail("could not remove fridge-side rollback fixture");
    if (r2_reality_remove_item("fridge_move_failure") != 0)
        fail("could not remove pocket copy of fridge rollback fixture");
    if (r2_reality_sleep(NAN) == 0)
        fail("non-finite sleep duration must be rejected");

    /* Exercise malformed and zero-fullness XML metrics against the real
       parser. The test root is disposable; append-only fixtures are isolated. */
    char metrics_path[1024];
    snprintf(metrics_path, sizeof(metrics_path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *metrics = fopen(metrics_path, "a");
    if (!metrics) fail("could not open temporary food metrics fixture");
    fputs("\n  <food name=\"burger\" fullness=\"100\" energy=\"10\" ingredients=\"bread,beef,cheese\" taste=\"savory,warm,salty\" />"
          "\n  <food name=\"zero_fullness\" fullness=\"0\" energy=\"0\" />"
          "\n  <food name=\"nan_fullness\" fullness=\"nan\" energy=\"10\" />"
          "\n  <food name=\"nan_energy\" fullness=\"20\" energy=\"nan\" />"
          "\n  <food name=\"fridge_move_failure\" fullness=\"45\" energy=\"4\" ingredients=\"new ingredient\" taste=\"new taste\" />"
          "\n  <food name=\"inventory_crash_move\" fullness=\"45\" energy=\"4\" ingredients=\"transfer ingredient\" taste=\"transfer taste\" />"
          "\n  <food name=\"inventory_crash_eat\" fullness=\"45\" energy=\"4\" ingredients=\"meal ingredient\" taste=\"meal taste\" />"
          "\n  <food name=\"inventory_crash_eat_commit\" fullness=\"45\" energy=\"4\" ingredients=\"meal ingredient\" taste=\"meal taste\" />\n",
          metrics);
    if (fclose(metrics) != 0) fail("could not close temporary food metrics fixture");

    /* Simulate a process crash after the fridge write but before the Reality
       object is removed. Restart must restore both databases to the old state. */
    if (r2_reality_add_item("inventory_crash_move", "Original pocket stack", "pockets", 2) != 0 ||
        r2_fridge_add_item("inventory_crash_move", "Original fridge stack", 3,
                           12.0, 1.0, "old ingredients", "old taste") != 0)
        fail("could not seed interrupted move-to-fridge fixture");
    if (setenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_ADD", "1", 1) != 0)
        fail("could not enable move-to-fridge crash injection");
    if (r2_reality_move_item("inventory_crash_move", "fridge") == 0)
        fail("injected move-to-fridge interruption must be reported as incomplete");
    unsetenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_ADD");
    r2_reality_shutdown();
    if (r2_reality_init() != 0)
        fail("Reality restart could not recover interrupted move-to-fridge transfer");

    char transfer_fridge_path[1024];
    snprintf(transfer_fridge_path, sizeof(transfer_fridge_path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *transfer_db = NULL;
    sqlite3_stmt *transfer_st = NULL;
    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not query recovered Reality item transfer");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT container,quantity FROM r2_reality_objects WHERE name='inventory_crash_move'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 0), "pockets") != 0 ||
        sqlite3_column_int(transfer_st, 1) != 2)
        fail("restart did not restore the original pocket stack after interrupted fridge move");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT COUNT(*) FROM r2_inventory_transfer_journal", -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW || sqlite3_column_int(transfer_st, 0) != 0)
        fail("move-to-fridge recovery did not clear its durable journal");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (sqlite3_open(transfer_fridge_path, &transfer_db) != SQLITE_OK)
        fail("could not query recovered fridge item transfer");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT description,quantity,fullness,energy FROM r2_fridge_items WHERE name='inventory_crash_move'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 0), "Original fridge stack") != 0 ||
        sqlite3_column_int(transfer_st, 1) != 3 ||
        fabs(sqlite3_column_double(transfer_st, 2) - 12.0) > 0.001 ||
        fabs(sqlite3_column_double(transfer_st, 3) - 1.0) > 0.001)
        fail("restart did not restore the original fridge stock metadata and quantity");
    sqlite3_finalize(transfer_st);
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    transfer_st = NULL;
    if (r2_reality_remove_item("inventory_crash_move") != 0)
        fail("could not remove recovered pocket transfer fixture");
    for (int i = 0; i < 3; ++i)
        if (r2_fridge_take("inventory_crash_move") != 0)
            fail("could not clear recovered fridge transfer fixture");
    if (r2_reality_remove_item("inventory_crash_move") != 0)
        fail("could not clear pocket copies from recovered fridge transfer fixture");

    /* Simulate a process crash after decrementing fridge stock but before the
       pocket addition. Restart must restore the fridge and keep pockets empty. */
    if (r2_fridge_add_item("inventory_crash_take", "Original take stock", 2,
                           12.0, 1.0, "take ingredients", "take taste") != 0)
        fail("could not seed interrupted fridge-take fixture");
    if (setenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_TAKE", "1", 1) != 0)
        fail("could not enable fridge-take crash injection");
    if (r2_fridge_take("inventory_crash_take") == 0)
        fail("injected fridge-take interruption must be reported as incomplete");
    unsetenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_TAKE");
    r2_reality_shutdown();
    if (r2_reality_init() != 0)
        fail("Reality restart could not recover interrupted fridge take");
    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not query recovered pocket take fixture");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT COUNT(*) FROM r2_reality_objects WHERE name='inventory_crash_take'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        sqlite3_column_int(transfer_st, 0) != 0)
        fail("restart left a pocket item from an interrupted fridge take");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not verify cleared fridge-take recovery journal");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT COUNT(*) FROM r2_inventory_transfer_journal", -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW || sqlite3_column_int(transfer_st, 0) != 0)
        fail("fridge-take recovery did not clear its durable journal");
    sqlite3_finalize(transfer_st);
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    transfer_st = NULL;
    if (sqlite3_open(transfer_fridge_path, &transfer_db) != SQLITE_OK)
        fail("could not query recovered fridge take fixture");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT description,quantity FROM r2_fridge_items WHERE name='inventory_crash_take'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 0), "Original take stock") != 0 ||
        sqlite3_column_int(transfer_st, 1) != 2)
        fail("restart did not restore the original fridge stock after interrupted take");
    sqlite3_finalize(transfer_st);
    sqlite3_close(transfer_db);
    if (r2_fridge_take("inventory_crash_take") != 0 ||
        r2_fridge_take("inventory_crash_take") != 0 ||
        r2_reality_remove_item("inventory_crash_take") != 0)
        fail("could not clean up recovered fridge-take fixture");

    /* A crash after fridge stock is decremented but before the meal transaction
       must restore stock, needs, starvation continuity, and food history. */
    if (r2_fridge_add_item("inventory_crash_eat", "Original meal stock", 1,
                           45.0, 4.0, "meal ingredient", "meal taste") != 0)
        fail("could not seed interrupted fridge-meal fixture");
    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not set baseline needs for interrupted fridge meal");
    if (sqlite3_exec(transfer_db,
        "UPDATE r2_reality_self SET hunger=30,satisfaction=20,seconds_since_meal=600,"
        "sleepiness=15,energy=50,last_tick=strftime('%s','now'),updated_at=CURRENT_TIMESTAMP WHERE id=1;"
        "DELETE FROM r2_food_experiences WHERE food_name='inventory_crash_eat';"
        "INSERT INTO r2_reality_self_facts(key,value,evidence) VALUES('starvation_72h_logged','yes','transfer recovery test') "
        "ON CONFLICT(key) DO UPDATE SET value='yes',evidence='transfer recovery test';",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not seed needs/history snapshot for interrupted fridge meal");
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (setenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_EAT_STOCK", "1", 1) != 0)
        fail("could not enable pre-meal crash injection");
    if (r2_reality_fridge_eat("inventory_crash_eat", -1.0) == 0)
        fail("injected pre-meal interruption must be reported as incomplete");
    unsetenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_EAT_STOCK");
    r2_reality_shutdown();
    if (r2_reality_init() != 0)
        fail("Reality restart could not recover interrupted fridge meal");

    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not verify needs after interrupted fridge meal");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT satisfaction,(SELECT COUNT(*) FROM r2_food_experiences WHERE food_name='inventory_crash_eat'),"
        "(SELECT value FROM r2_reality_self_facts WHERE key='starvation_72h_logged'),"
        "(SELECT COUNT(*) FROM r2_inventory_transfer_journal) FROM r2_reality_self WHERE id=1",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        sqlite3_column_double(transfer_st, 0) >= 30.0 ||
        sqlite3_column_int(transfer_st, 1) != 0 ||
        !sqlite3_column_text(transfer_st, 2) ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 2), "yes") != 0 ||
        sqlite3_column_int(transfer_st, 3) != 0)
        fail("pre-meal crash did not restore needs, starvation fact, food history, and journal");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (sqlite3_open(transfer_fridge_path, &transfer_db) != SQLITE_OK)
        fail("could not verify stock after interrupted fridge meal");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT description,quantity FROM r2_fridge_items WHERE name='inventory_crash_eat'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 0), "Original meal stock") != 0 ||
        sqlite3_column_int(transfer_st, 1) != 1)
        fail("pre-meal crash did not restore the original fridge stock");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (r2_fridge_take("inventory_crash_eat") != 0 ||
        r2_reality_remove_item("inventory_crash_eat") != 0)
        fail("could not clean up pre-meal crash fixture");

    /* Once the needs/food-history transaction commits the journal phase in the
       same SQLite transaction, recovery must keep the meal rather than refund it. */
    if (r2_fridge_add_item("inventory_crash_eat_commit", "Committed meal stock", 1,
                           45.0, 4.0, "meal ingredient", "meal taste") != 0)
        fail("could not seed committed fridge-meal fixture");
    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not set baseline needs for committed fridge meal");
    if (sqlite3_exec(transfer_db,
        "UPDATE r2_reality_self SET hunger=30,satisfaction=20,seconds_since_meal=600,"
        "sleepiness=15,energy=50,last_tick=strftime('%s','now'),updated_at=CURRENT_TIMESTAMP WHERE id=1;"
        "DELETE FROM r2_food_experiences WHERE food_name='inventory_crash_eat_commit';"
        "INSERT INTO r2_reality_self_facts(key,value,evidence) VALUES('starvation_72h_logged','yes','commit recovery test') "
        "ON CONFLICT(key) DO UPDATE SET value='yes',evidence='commit recovery test';",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not seed baseline for committed fridge meal");
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (setenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_EAT_COMMIT", "1", 1) != 0)
        fail("could not enable committed-meal crash injection");
    if (r2_reality_fridge_eat("inventory_crash_eat_commit", -1.0) == 0)
        fail("injected post-commit interruption must be reported as incomplete");
    unsetenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_EAT_COMMIT");
    r2_reality_shutdown();
    if (r2_reality_init() != 0)
        fail("Reality restart could not recover committed fridge meal");

    if (sqlite3_open(R2_HOME "/r2_reality.db", &transfer_db) != SQLITE_OK)
        fail("could not verify committed meal after restart");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT satisfaction,(SELECT COUNT(*) FROM r2_food_experiences WHERE food_name='inventory_crash_eat_commit'),"
        "(SELECT value FROM r2_reality_self_facts WHERE key='starvation_72h_logged'),"
        "(SELECT COUNT(*) FROM r2_inventory_transfer_journal) FROM r2_reality_self WHERE id=1",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        sqlite3_column_double(transfer_st, 0) < 40.0 ||
        sqlite3_column_int(transfer_st, 1) != 1 ||
        !sqlite3_column_text(transfer_st, 2) ||
        strcmp((const char *)sqlite3_column_text(transfer_st, 2), "no") != 0 ||
        sqlite3_column_int(transfer_st, 3) != 0)
        fail("post-commit recovery rolled back a meal or retained its journal");
    sqlite3_finalize(transfer_st);
    transfer_st = NULL;
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (sqlite3_open(transfer_fridge_path, &transfer_db) != SQLITE_OK)
        fail("could not verify committed meal stock after restart");
    if (sqlite3_prepare_v2(transfer_db,
        "SELECT COUNT(*) FROM r2_fridge_items WHERE name='inventory_crash_eat_commit'",
        -1, &transfer_st, NULL) != SQLITE_OK ||
        sqlite3_step(transfer_st) != SQLITE_ROW ||
        sqlite3_column_int(transfer_st, 0) != 0)
        fail("post-commit recovery incorrectly restored consumed fridge stock");
    sqlite3_finalize(transfer_st);
    sqlite3_close(transfer_db);
    transfer_db = NULL;
    if (r2_fridge_add_item("invalid_negative_metric", "invalid test stock", 1,
                           -1.0, 10.0, NULL, NULL) == 0)
        fail("fridge API must reject negative fullness instead of substituting defaults");
    if (r2_fridge_add_item("invalid_energy_metric", "invalid test stock", 1,
                           50.0, 101.0, NULL, NULL) == 0)
        fail("fridge API must reject out-of-range energy instead of substituting defaults");
    if (r2_fridge_add_item("invalid_quantity", "invalid test stock", 0,
                           50.0, 10.0, NULL, NULL) == 0)
        fail("direct fridge API must reject a non-positive stock quantity");
    if (r2_fridge_add_item("overflow_fridge", "Quantity boundary test", INT_MAX,
                           20.0, 2.0, "", "") != 0)
        fail("fridge API should accept a representable maximum stock quantity");
    if (r2_fridge_add_item("overflow_fridge", "Quantity boundary test", 1,
                           20.0, 2.0, "", "") == 0)
        fail("fridge API must reject a stock quantity overflow");
    char fridge_overflow_path[1024];
    snprintf(fridge_overflow_path, sizeof(fridge_overflow_path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *fridge_overflow_db = NULL;
    sqlite3_stmt *fridge_overflow_st = NULL;
    if (sqlite3_open(fridge_overflow_path, &fridge_overflow_db) != SQLITE_OK)
        fail("could not query fridge quantity overflow regression");
    if (sqlite3_prepare_v2(fridge_overflow_db,
        "SELECT quantity FROM r2_fridge_items WHERE name='overflow_fridge'",
        -1, &fridge_overflow_st, NULL) != SQLITE_OK ||
        sqlite3_step(fridge_overflow_st) != SQLITE_ROW ||
        sqlite3_column_int64(fridge_overflow_st, 0) != INT_MAX)
        fail("overflowing fridge addition changed authoritative stock quantity");
    sqlite3_finalize(fridge_overflow_st);
    if (sqlite3_exec(fridge_overflow_db,
        "DELETE FROM r2_fridge_items WHERE name='overflow_fridge'",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not clean up fridge quantity overflow fixture");
    sqlite3_close(fridge_overflow_db);
    char fridge_overflow_mirror[1200];
    snprintf(fridge_overflow_mirror, sizeof(fridge_overflow_mirror),
             "%s/fridge/overflow_fridge.r2item", R2_ROOT);
    (void)unlink(fridge_overflow_mirror);
    if (r2_reality_eat("nan_fullness", -1.0) == 0)
        fail("non-finite fullness metric must be rejected instead of corrupting needs");

    /* A malformed energy bonus must fall back to the finite fullness-derived
       bonus rather than propagating NaN into the persistent energy column. */
    if (r2_reality_add_item("nan_energy", "Finite fallback metric test", "pockets", 1) != 0)
        fail("could not add tracked item for malformed energy metric test");
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *metric_db = NULL;
    sqlite3_stmt *metric_st = NULL;
    if (sqlite3_open(path, &metric_db) != SQLITE_OK)
        fail("could not open Reality DB for malformed energy metric test");
    if (sqlite3_exec(metric_db, "UPDATE r2_reality_self SET energy=50,hunger=30 WHERE id=1",
                     NULL, NULL, NULL) != SQLITE_OK)
        fail("could not set energy baseline for malformed metric test");
    sqlite3_close(metric_db);
    if (r2_reality_eat("nan_energy", -1.0) != 0)
        fail("finite fullness with malformed energy bonus should use safe default");
    if (sqlite3_open(path, &metric_db) != SQLITE_OK)
        fail("could not reopen Reality DB after malformed energy metric test");
    if (sqlite3_prepare_v2(metric_db, "SELECT energy FROM r2_reality_self WHERE id=1",
                           -1, &metric_st, NULL) != SQLITE_OK)
        fail("could not query energy after malformed metric test");
    if (sqlite3_step(metric_st) != SQLITE_ROW ||
        sqlite3_column_double(metric_st, 0) < 51.9 ||
        sqlite3_column_double(metric_st, 0) > 52.1)
        fail("malformed energy bonus must fall back to fullness-derived finite energy");
    sqlite3_finalize(metric_st);
    sqlite3_close(metric_db);
    /* The successful meal already consumed the single tracked test item. */

    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=100,satisfaction=100,seconds_since_meal=0,"
        "sleepiness=0,energy=100,last_tick=? WHERE id=1", -1, &st, NULL) != SQLITE_OK)
        fail("could not prepare controlled starting state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) - 3600);
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set controlled starting state");
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (r2_reality_tick() != 0) fail("one-hour needs tick failed");
    double hunger, sleepiness, energy, satisfaction, since_meal;
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 65.5 || hunger > 67.5) fail("fullness should decrease by about 33.3 points per hour and reach zero in roughly three hours");
    if (sleepiness < 12.0 || sleepiness > 13.0) fail("sleepiness should rise about 12.5 points per hour");
    if (energy < 89.0 || energy > 91.0) fail("energy should fall about 10 points per hour");
    if (satisfaction < 23.5 || satisfaction > 25.0) fail("satiety should fall about 75.8 points per hour at its independent configured rate");

    if (r2_reality_sleep(8.0) != 0) fail("eight-hour sleep transition failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 0.1) fail("eight modeled hours without food should drain the fullness meter to zero");
    if (satisfaction > 0.1) fail("eight modeled hours without a qualifying meal should deplete satiety to zero");
    if (sleepiness > 1.0) fail("eight hours of sleep should restore sleepiness to zero");
    if (energy < 89.0 || energy > 91.0) fail("sleep should restore energy without exceeding its cap");
    if (since_meal < 32300.0 || since_meal > 32500.0) fail("sleep must advance elapsed time since meal");

    /* Three modeled hours without food should drain the fullness meter to
       zero; this directly guards the intended time scale. */
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality DB for three-hour fullness test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=100,satisfaction=100,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare three-hour fullness state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) - 10800);
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set three-hour fullness state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (r2_reality_tick() != 0) fail("three-hour fullness tick failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 0.1) fail("fullness should reach zero after roughly three modeled hours without food");

    /* Make the fridge's first-choice item explicit so the end-to-end test
       can prove one unit is consumed, not merely that hunger was reset. */
    char fridge_path[1024];
    snprintf(fridge_path, sizeof(fridge_path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *fridge = NULL;
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not open fridge database");
    if (r2_fridge_add_item("nan_fullness_stock", "Invalid metric test", 1, NAN, 10.0, "", "") == 0)
        fail("fridge must reject NaN fullness before persisting stock");
    if (r2_fridge_add_item("nan_energy_stock", "Invalid metric test", 1, 20.0, NAN, "", "") == 0)
        fail("fridge must reject NaN energy before persisting stock");
    if (sqlite3_exec(fridge,
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) "
        "VALUES('apple','Regression-test food stock',3,80,8,'apple','sweet') "
        "ON CONFLICT(name) DO UPDATE SET quantity=3,fullness=80,energy=8",
        NULL, NULL, NULL) != SQLITE_OK) fail("could not seed known fridge food stock");
    if (sqlite3_exec(fridge,
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) "
        "VALUES('aaa_empty','Non-nutritive regression stock',2,0,0,'','') "
        "ON CONFLICT(name) DO UPDATE SET quantity=2,fullness=0,energy=0",
        NULL, NULL, NULL) != SQLITE_OK) fail("could not seed non-nutritive fridge stock");
    sqlite3_close(fridge);
    if (r2_reality_autonomous_food_available() != 1)
        fail("accessible fridge stock with positive fullness must count as available food");

    if (r2_reality_fridge_eat("aaa_empty", -1.0) == 0)
        fail("zero-fullness fridge stock must not count as a meal");
    if (r2_reality_fridge_eat("aaa_empty", 50.0) == 0)
        fail("caller-supplied fullness must not override authoritative zero-fullness stock");
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK)
        fail("could not reopen fridge after zero-fullness eat attempt");
    if (sqlite3_prepare_v2(fridge, "SELECT quantity FROM r2_fridge_items WHERE name='aaa_empty'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query zero-fullness stock after eat attempt");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 2)
        fail("failed non-nutritive meal attempt must restore the original fridge quantity");
    sqlite3_finalize(st);
    sqlite3_close(fridge);

    /* Away from home, room food and the fridge are both inaccessible. */
    if (r2_reality_add_item("burger", "Room-only regression food", "room", 1) != 0)
        fail("could not add room-only test food");
    if (r2_reality_set_location(0) != 0) fail("could not move simulated location away from home");
    if (r2_reality_autonomous_food_available() != 0)
        fail("room food and fridge stock must not count as accessible while away");

    if (r2_reality_set_location(1) != 0) fail("could not return home after away test");
    if (r2_reality_remove_item("burger") != 0) fail("could not remove room-only regression food");

    /* Accessible portable food must skip a configured zero-fullness item,
       then consume a useful tracked item with exactly one unit removed. */
    if (r2_reality_add_item("zero_fullness", "Non-nutritive regression item", "pockets", 1) != 0)
        fail("could not add zero-fullness tracked item to pockets");
    if (r2_reality_add_item("burger", "Explicit regression-test food", "pockets", 2) != 0)
        fail("could not add explicit tracked food to pockets");
    if (r2_reality_add_item("unlisted_food", "No configured food metric", "pockets", 1) != 0)
        fail("could not add an ordinary unlisted object to pockets");
    if (r2_reality_add_item("unlisted_food", "No configured food metric", "fridge", 1) == 0)
        fail("unlisted item must not become nutritious fridge stock by default");
    if (r2_reality_move_item("unlisted_food", "fridge") == 0)
        fail("moving an unconfigured item into the fridge must not invent nutrition");
    if (r2_reality_eat("zero_fullness", -1.0) == 0)
        fail("zero-fullness tracked stock must not count as a meal");
    if (r2_reality_eat("zero_fullness", 50.0) == 0)
        fail("caller-supplied fullness must not override tracked food metrics");
    if (r2_reality_eat("unlisted_food", 50.0) == 0)
        fail("unlisted tracked food must not be accepted from a guessed fullness value");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for tracked-food test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=40,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare tracked-food hunger state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set tracked-food hunger state");
    sqlite3_finalize(st);
    if (sqlite3_exec(db,
        "CREATE TRIGGER smoke_fail_food_experience BEFORE INSERT ON r2_food_experiences "
        "BEGIN SELECT RAISE(ABORT,'intentional rollback test'); END",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not install transaction rollback test trigger");
    sqlite3_close(db);

    /* Force the final meal write to fail after needs and stock were updated.
       The transaction must roll both changes back together. */
    if (r2_reality_eat("burger", -1.0) == 0)
        fail("injected meal persistence failure should be reported");
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not reopen Reality database after rollback test");
    if (sqlite3_prepare_v2(db,
        "SELECT hunger,seconds_since_meal FROM r2_reality_self WHERE id=1",
        -1, &st, NULL) != SQLITE_OK)
        fail("could not query needs after rollback test");
    if (sqlite3_step(st) != SQLITE_ROW ||
        sqlite3_column_double(st, 0) < 39.8 ||
        sqlite3_column_double(st, 0) > 40.1 ||
        sqlite3_column_double(st, 1) < 9990.0)
        fail("failed meal must roll back fullness and meal-clock changes");
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "SELECT quantity FROM r2_reality_objects WHERE name='burger'",
        -1, &st, NULL) != SQLITE_OK)
        fail("could not query tracked stock after rollback test");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 2)
        fail("failed meal must roll back tracked inventory decrement");
    sqlite3_finalize(st);
    if (sqlite3_exec(db, "DROP TRIGGER smoke_fail_food_experience",
                     NULL, NULL, NULL) != SQLITE_OK)
        fail("could not remove transaction rollback test trigger");
    sqlite3_close(db);

    if (r2_reality_remove_item("burger") != 0) fail("could not remove remaining regression-test food");
    if (r2_reality_remove_item("zero_fullness") != 0) fail("could not remove zero-fullness regression item");
    if (r2_reality_remove_item("unlisted_food") != 0) fail("could not remove unlisted-object regression item");

    /* Empty fridge is a real state, not a cue to spawn a burger. */
    if (r2_reality_set_location(1) != 0) fail("could not return simulated location home");
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not reopen fridge for empty test");
    if (sqlite3_exec(fridge, "DELETE FROM r2_fridge_items", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not empty fridge for no-invention test");
    sqlite3_close(fridge);
    char *empty_fridge_context = r2_fridge_context();
    if (!empty_fridge_context ||
        strstr(empty_fridge_context, "generated automatically") ||
        strstr(empty_fridge_context, "automatically generates") ||
        !strstr(empty_fridge_context, "stays empty") ||
        !strstr(empty_fridge_context, "fridge is empty"))
        fail("fridge context must not tell the model that empty stock respawns");
    free(empty_fridge_context);
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for empty-fridge test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=40,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare empty-fridge hunger state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set empty-fridge hunger state");
    sqlite3_finalize(st);
    sqlite3_close(db);


    /* Persistence boundary: needs survive a full Reality shutdown/init cycle. */
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for restart test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=23.5,satisfaction=77,seconds_since_meal=123,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare restart persistence state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set restart persistence state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    /* Seed a record matching the exact legacy auto-generated marker.
       Reinitialization should clean this phantom stock, not real inventory. */
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not open fridge for legacy cleanup test");
    if (sqlite3_exec(fridge,
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste) "
        "VALUES('burger','Guaranteed filling burger generated because the fridge was empty',1,100,10,'bread,beef,cheese','savory') "
        "ON CONFLICT(name) DO UPDATE SET description='Guaranteed filling burger generated because the fridge was empty',quantity=1",
        NULL, NULL, NULL) != SQLITE_OK) fail("could not seed legacy phantom-food record");
    sqlite3_close(fridge);
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not open Reality database for legacy tracked-food cleanup test");
    if (sqlite3_exec(db,
        "INSERT INTO r2_reality_objects(name,description,quantity,container,owner) "
        "VALUES('legacy_phantom_burger','Guaranteed filling burger generated because the fridge was empty',1,'pockets','R2') "
        "ON CONFLICT(name) DO UPDATE SET description='Guaranteed filling burger generated because the fridge was empty',quantity=1,container='pockets';"
        "INSERT INTO r2_reality_objects(name,description,quantity,container,owner) "
        "VALUES('burger','User-owned burger; legitimate inventory',1,'pockets','R2') "
        "ON CONFLICT(name) DO UPDATE SET description='User-owned burger; legitimate inventory',quantity=1,container='pockets'",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not seed legacy and legitimate burger inventory");
    sqlite3_close(db);
    r2_reality_shutdown();
    if (r2_reality_init() != 0) fail("Reality restart failed");
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not reopen fridge after legacy cleanup");
    if (sqlite3_prepare_v2(fridge,
        "SELECT COUNT(*) FROM r2_fridge_items WHERE description='Guaranteed filling burger generated because the fridge was empty'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query legacy phantom-food records");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 0)
        fail("restart must remove only legacy auto-generated phantom food");
    sqlite3_finalize(st);
    sqlite3_close(fridge);
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not reopen Reality database after legacy tracked-food cleanup");
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM r2_reality_objects WHERE description='Guaranteed filling burger generated because the fridge was empty'",
        -1, &st, NULL) != SQLITE_OK)
        fail("could not query legacy phantom-food copies in tracked inventory");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 0)
        fail("restart must remove legacy phantom-food copies from tracked inventory");
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM r2_reality_objects WHERE name='burger' "
        "AND description='User-owned burger; legitimate inventory' AND quantity=1",
        -1, &st, NULL) != SQLITE_OK)
        fail("could not verify legitimate burger survived legacy cleanup");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1)
        fail("legacy cleanup must preserve legitimate user-owned burger inventory");
    sqlite3_finalize(st);
    sqlite3_close(db);
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 23.4 || hunger > 23.6) fail("hunger should persist across restart");
    if (satisfaction < 76.9 || satisfaction > 77.1) fail("satisfaction should persist across restart");
    if (since_meal < 122.0 || since_meal > 124.0) fail("elapsed time since meal should persist across restart");

    /* At the 72-hour starvation marker, status must not still say only
       "very hungry" while the prolonged-starvation event is recorded. */
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for starvation status");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=5,seconds_since_meal=259200,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare prolonged-starvation state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set prolonged-starvation state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    char *status = r2_reality_status();
    if (!status || !strstr(status, "(starving)"))
        fail("hunger at the 72-hour starvation threshold should be labeled starving");
    free(status);

    /* Gaps longer than a week must still advance needs and world time. The
       former seven-day guard discarded the entire interval and could prevent
       the 72-hour starvation marker from ever being recorded after downtime. */
    sqlite3_int64 world_before = read_world_elapsed();
    seed_eight_day_gap(path);
    if (r2_reality_tick() != 0) fail("runtime tick rejected an eight-day elapsed gap");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 0.1) fail("runtime catch-up must drain fullness after an eight-day gap");
    if (since_meal < 72.0 * 3600.0) fail("runtime catch-up must preserve elapsed time since meal");
    if (read_world_elapsed() - world_before < 8 * 86400 - 5)
        fail("runtime catch-up must advance modeled world time across a long gap");
    if (!starvation_flag_is_yes())
        fail("runtime catch-up must record prolonged starvation after a long gap");

    r2_reality_shutdown();
    world_before = read_world_elapsed();
    seed_eight_day_gap(path);
    if (r2_reality_init() != 0) fail("Reality restart failed for long-gap catch-up test");
    if (r2_reality_tick() != 0) fail("post-startup tick failed after long-gap catch-up");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 0.1) fail("startup catch-up must drain fullness after an eight-day gap");
    if (since_meal < 72.0 * 3600.0) fail("startup catch-up must preserve elapsed time since meal");
    if (read_world_elapsed() - world_before < 8 * 86400 - 5)
        fail("startup catch-up must advance modeled world time across a long gap");
    if (!starvation_flag_is_yes())
        fail("startup catch-up must record prolonged starvation after a long gap");

    /* Inject a failure after sleep has advanced fullness decay but before it restores
       sleepiness. The enclosing transaction must roll the entire transition
       back rather than persist a half-slept body state. */
    if (r2_reality_tick() != 0) fail("pre-failure tick failed");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality DB for rollback test");
    if (sqlite3_exec(db, "UPDATE r2_reality_self SET last_tick=CAST(strftime('%s','now') AS INTEGER)+100 WHERE id=1", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not isolate rollback test from wall-clock second boundaries");
    double before_hunger = 0.0, before_sleepiness = 0.0, before_energy = 0.0;
    char before_elapsed[64] = {0};
    if (sqlite3_prepare_v2(db,
        "SELECT hunger,sleepiness,energy FROM r2_reality_self WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare rollback baseline");
    if (sqlite3_step(st) != SQLITE_ROW) fail("rollback baseline row missing");
    before_hunger = sqlite3_column_double(st, 0);
    before_sleepiness = sqlite3_column_double(st, 1);
    before_energy = sqlite3_column_double(st, 2);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "SELECT value FROM r2_reality_meta WHERE key='world_elapsed_seconds'",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare elapsed-time baseline");
    if (sqlite3_step(st) != SQLITE_ROW) fail("elapsed-time baseline missing");
    snprintf(before_elapsed, sizeof(before_elapsed), "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (sqlite3_exec(db,
        "CREATE TRIGGER inject_sleep_failure BEFORE UPDATE ON r2_reality_self "
        "WHEN NEW.sleepiness < OLD.sleepiness "
        "BEGIN SELECT RAISE(ABORT,'injected sleep failure'); END",
        NULL, NULL, NULL) != SQLITE_OK) fail("could not create injected sleep failure");
    sqlite3_close(db);
    if (r2_reality_sleep(1.0) == 0)
        fail("sleep must fail when the restorative state update fails");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality DB after rollback test");
    if (sqlite3_exec(db, "DROP TRIGGER inject_sleep_failure", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not remove injected sleep failure");
    if (sqlite3_prepare_v2(db,
        "SELECT hunger,sleepiness,energy FROM r2_reality_self WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not query needs after rollback");
    if (sqlite3_step(st) != SQLITE_ROW ||
        fabs(sqlite3_column_double(st, 0) - before_hunger) > 0.0001 ||
        fabs(sqlite3_column_double(st, 1) - before_sleepiness) > 0.0001 ||
        fabs(sqlite3_column_double(st, 2) - before_energy) > 0.0001)
        fail("failed sleep must roll back every needs column");
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "SELECT value FROM r2_reality_meta WHERE key='world_elapsed_seconds'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query elapsed time after rollback");
    if (sqlite3_step(st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(st, 0), before_elapsed) != 0)
        fail("failed sleep must roll back modeled world elapsed time");
    sqlite3_finalize(st);
    sqlite3_close(db);

    /* A failed optional food-history query must not erase the rest of the
       combined Reality context. Missing data is labeled explicitly. */
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not open Reality DB for context resilience test");
    if (sqlite3_exec(db, "DROP TABLE r2_food_experiences", NULL, NULL, NULL) != SQLITE_OK)
        fail("could not inject optional food-history query failure");
    sqlite3_close(db);
    char *context = r2_reality_context();
    if (!context || !strstr(context, "PERSISTENT REALITY CONTEXT") ||
        !strstr(context, "Food-experience history unavailable") ||
        !strstr(context, "SELF CONTINUITY") || !strstr(context, "R2'S ROOM")) {
        free(context);
        fail("one unavailable optional subsystem must not discard all Reality context");
    }
    free(context);

    /* Simulate an upgrade from the old increasing-hunger scale. The migration
       must invert it once (20 -> 80 fullness), then remain idempotent. */
    r2_reality_shutdown();
    if (sqlite3_open(path, &db) != SQLITE_OK)
        fail("could not open Reality DB for hunger semantics migration test");
    if (sqlite3_exec(db,
        "UPDATE r2_reality_self SET hunger=20 WHERE id=1;"
        "UPDATE r2_reality_meta SET value='no' WHERE key='hunger_fullness_semantics_v2';",
        NULL, NULL, NULL) != SQLITE_OK)
        fail("could not seed old hunger semantics fixture");
    sqlite3_close(db);
    if (r2_reality_init() != 0) fail("hunger semantics migration restart failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 79.9 || hunger > 80.1)
        fail("old hunger value must be inverted to fullness exactly once during migration");
    r2_reality_shutdown();
    if (r2_reality_init() != 0) fail("second hunger semantics restart failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 79.9 || hunger > 80.1)
        fail("hunger semantics migration must not invert state again on later restarts");
    r2_reality_shutdown();

    puts("reality needs smoke passed");
    return 0;
}
