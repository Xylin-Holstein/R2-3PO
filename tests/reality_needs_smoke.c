#include "Reality.h"

#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    if (since_meal < 28700.0 || since_meal > 28900.0) fail("sleep must advance elapsed time since meal");

    r2_reality_shutdown();
    puts("reality needs smoke passed");
    return 0;
}
