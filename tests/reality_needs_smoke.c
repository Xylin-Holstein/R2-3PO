#include "Reality.h"

#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int main(void)
{
    if (r2_reality_init() != 0) fail("Reality initialization failed");

    /* Exercise malformed and zero-fullness XML metrics against the real
       parser. The test root is disposable; append-only fixtures are isolated. */
    char metrics_path[1024];
    snprintf(metrics_path, sizeof(metrics_path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *metrics = fopen(metrics_path, "a");
    if (!metrics) fail("could not open temporary food metrics fixture");
    fputs("\n  <food name=\"zero_fullness\" fullness=\"0\" energy=\"0\" />"
          "\n  <food name=\"nan_fullness\" fullness=\"nan\" energy=\"10\" />"
          "\n  <food name=\"nan_energy\" fullness=\"20\" energy=\"nan\" />\n",
          metrics);
    if (fclose(metrics) != 0) fail("could not close temporary food metrics fixture");
    if (r2_reality_eat("nan_fullness", -1.0) == 0)
        fail("non-finite fullness metric must be rejected instead of corrupting needs");

    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_reality.db", R2_HOME);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not open Reality database");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=0,satisfaction=100,seconds_since_meal=0,"
        "sleepiness=0,energy=100,last_tick=? WHERE id=1", -1, &st, NULL) != SQLITE_OK)
        fail("could not prepare controlled starting state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL) - 3600);
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set controlled starting state");
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (r2_reality_tick() != 0) fail("one-hour needs tick failed");
    double hunger, sleepiness, energy, satisfaction, since_meal;
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 1.20 || hunger > 1.35) fail("hunger should rise about 1.26 points per hour");
    if (sleepiness < 12.0 || sleepiness > 13.0) fail("sleepiness should rise about 12.5 points per hour");
    if (energy < 89.0 || energy > 91.0) fail("energy should fall about 10 points per hour");
    if (satisfaction < 98.6 || satisfaction > 98.9) fail("satiety should fall at the hunger rate");

    if (r2_reality_sleep(8.0) != 0) fail("eight-hour sleep transition failed");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger < 11.0 || hunger > 12.0) fail("eight hours of sleep should add about 10.1 hunger points");
    if (sleepiness > 1.0) fail("eight hours of sleep should restore sleepiness to zero");
    if (energy < 89.0 || energy > 91.0) fail("sleep should restore energy without exceeding its cap");
    if (since_meal < 32300.0 || since_meal > 32500.0) fail("sleep must advance elapsed time since meal");

    /* Make the fridge's first-choice item explicit so the end-to-end test
       can prove one unit is consumed, not merely that hunger was reset. */
    char fridge_path[1024];
    snprintf(fridge_path, sizeof(fridge_path), "%s/r2_fridge.db", R2_HOME);
    sqlite3 *fridge = NULL;
    if (sqlite3_open(fridge_path, &fridge) != SQLITE_OK) fail("could not open fridge database");
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

    /* Simulate the autonomous cycle with no model-selected action: hunger
       reaches 60, verified food is consumed, stock decrements, and needs reset. */
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=60,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare hungry fallback state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set hungry fallback state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (r2_reality_autonomous_feed_if_needed() != 1)
        fail("verified fridge food should be consumed when hunger reaches 60");
    read_needs(&hunger, &sleepiness, &energy, &satisfaction, &since_meal);
    if (hunger > 1.0) fail("verified food should reset high hunger");
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
       The safeguard must preserve both inventories and leave hunger high. */
    if (r2_reality_add_item("burger", "Room-only regression food", "room", 1) != 0)
        fail("could not add room-only test food");
    if (r2_reality_set_location(0) != 0) fail("could not move simulated location away from home");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for away test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=60,seconds_since_meal=10000,last_tick=? WHERE id=1",
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
    if (hunger < 59.0) fail("away fallback must not fake eating or reset hunger");
    if (r2_reality_set_location(1) != 0) fail("could not return home after away test");
    if (r2_reality_remove_item("burger") != 0) fail("could not remove room-only regression food");

    /* Accessible portable food must skip a configured zero-fullness item,
       then consume a useful tracked item with exactly one unit removed. */
    if (r2_reality_add_item("zero_fullness", "Non-nutritive regression item", "pockets", 1) != 0)
        fail("could not add zero-fullness tracked item to pockets");
    if (r2_reality_add_item("burger", "Explicit regression-test food", "pockets", 2) != 0)
        fail("could not add explicit tracked food to pockets");
    if (r2_reality_eat("zero_fullness", -1.0) == 0)
        fail("zero-fullness tracked stock must not count as a meal");
    if (sqlite3_open(path, &db) != SQLITE_OK) fail("could not reopen Reality database for tracked-food test");
    if (sqlite3_prepare_v2(db,
        "UPDATE r2_reality_self SET hunger=60,seconds_since_meal=10000,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare tracked-food hunger state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set tracked-food hunger state");
    sqlite3_finalize(st);
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
        "UPDATE r2_reality_self SET hunger=60,seconds_since_meal=10000,last_tick=? WHERE id=1",
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
    if (hunger < 59.0) fail("empty-fridge fallback must not pretend R2 ate");

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
        "UPDATE r2_reality_self SET hunger=91,seconds_since_meal=259200,last_tick=? WHERE id=1",
        -1, &st, NULL) != SQLITE_OK) fail("could not prepare prolonged-starvation state");
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) fail("could not set prolonged-starvation state");
    sqlite3_finalize(st);
    sqlite3_close(db);
    char *status = r2_reality_status();
    if (!status || !strstr(status, "(starving)"))
        fail("hunger at the 72-hour starvation threshold should be labeled starving");
    free(status);

    r2_reality_shutdown();
    puts("reality needs smoke passed");
    return 0;
}
