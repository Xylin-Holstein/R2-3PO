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

int main(void)
{
    seed_legacy_fridge_schema();
    if (r2_reality_init() != 0) fail("Reality initialization failed");
    verify_legacy_fridge_migration();
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
          "\n  <food name=\"fridge_move_failure\" fullness=\"45\" energy=\"4\" ingredients=\"new ingredient\" taste=\"new taste\" />\n",
          metrics);
    if (fclose(metrics) != 0) fail("could not close temporary food metrics fixture");
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
    if (satisfaction < 23.5 || satisfaction > 25.0) fail("satiety should fall about 75.8 points per hour at the 4.75-second rate");

    if (r2_reality_sleep(8.0) != 0) fail("eight-hour sleep transition failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 0.1) fail("eight modeled hours without food should drain the fullness meter to zero");
    if (satisfaction > 0.1) fail("eight modeled hours without a qualifying meal should deplete satiety to zero");
    if (sleepiness > 1.0) fail("eight hours of sleep should restore sleepiness to zero");
    if (energy < 89.0 || energy > 91.0) fail("sleep should restore energy without exceeding its cap");
    if (since_meal < 32300.0 || since_meal > 32500.0) fail("sleep must advance elapsed time since meal");

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

    /* Simulate the autonomous cycle with no model-selected action: fullness
       is low, verified food is consumed, stock decrements, and fullness rises. */
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=40,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare hungry fallback state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set hungry fallback state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (r2_reality_autonomous_feed_if_needed() != 1)
        fail("verified fridge food should be consumed when fullness falls below 50");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 99.0) fail("verified food should restore fullness according to its configured metric");
    if (since_meal > 5.0) fail("eating should reset time since meal");

    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not reopen fridge database");
    if (sqlite3_prepare_v2(fridge, "SELECT quantity FROM r2_fridge_items WHERE name='apple'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query consumed fridge stock");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 2)
        fail("autonomous feeding must decrement authoritative fridge quantity by one");
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(fridge, "SELECT quantity FROM r2_fridge_items WHERE name='aaa_empty'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query non-nutritive fridge stock");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 2)
        fail("autonomous feeding must skip stock with zero fullness");
    sqlite3_finalize(st);
    sqlite3_close(fridge);

    /* Away from home, room food and the fridge are both inaccessible.
       The safeguard must preserve both inventories and leave fullness low. */
    if (r2_reality_add_item("burger", "Room-only regression food", "room", 1) != 0)
        fail("could not add room-only test food");
    if (r2_reality_set_location(0) != 0) fail("could not move simulated location away from home");
    if (r2_reality_autonomous_food_available() != 0)
        fail("room food and fridge stock must not count as accessible while away");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for away test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=40,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare away hunger state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set away hunger state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (r2_reality_autonomous_feed_if_needed() != 0)
        fail("away fallback should not consume home fridge stock");
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not reopen fridge for away check");
    if (sqlite3_prepare_v2(fridge, "SELECT quantity FROM r2_fridge_items WHERE name='apple'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query away fridge stock");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 2)
        fail("away fallback must leave fridge stock unchanged");
    sqlite3_finalize(st);
    sqlite3_close(fridge);
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for inaccessible-item check");
    if (sqlite3_prepare_v2(db, "SELECT quantity FROM r2_reality_objects WHERE name='burger'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query inaccessible room food");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1)
        fail("away fallback must not consume room-only food");
    sqlite3_finalize(st);
    sqlite3_close(db);
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 40.1) fail("away fallback must not fake eating or increase fullness");
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
        sqlite3_column_double(st, 0) < 59.8 ||
        sqlite3_column_double(st, 1) < 9990.0)
        fail("failed meal must roll back hunger and meal-clock changes");
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

    if (r2_reality_autonomous_feed_if_needed() != 1)
        fail("accessible tracked food should be consumed before fridge stock");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database to verify tracked stock");
    if (sqlite3_prepare_v2(db, "SELECT quantity FROM r2_reality_objects WHERE name='burger'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query tracked food quantity");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1)
        fail("tracked feeding must decrement quantity by exactly one");
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db, "SELECT quantity FROM r2_reality_objects WHERE name='zero_fullness'",
        -1, &st, NULL) != SQLITE_OK) fail("could not query zero-fullness tracked item");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 1)
        fail("autonomous feeding must not consume a zero-fullness tracked item");
    sqlite3_finalize(st);
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
    if (r2_reality_autonomous_feed_if_needed() != 0)
        fail("empty fridge should report no food rather than inventing stock");
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not reopen empty fridge");
    if (sqlite3_prepare_v2(fridge, "SELECT COUNT(*) FROM r2_fridge_items",
        -1, &st, NULL) != SQLITE_OK) fail("could not verify empty fridge");
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) != 0)
        fail("autonomous fallback must not create phantom food in an empty fridge");
    sqlite3_finalize(st);
    sqlite3_close(fridge);
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 40.1) fail("empty-fridge fallback must not pretend R2 ate or increase fullness");

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

    r2_reality_shutdown();
    puts("reality needs smoke passed");
    return 0;
}
