/* Exercise the production fridge migration and write paths against a legacy
   table whose qty alias remains NOT NULL after additive migration. */
#include "../Reality.c"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/r2-fridge-legacy-smoke-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(sqlite3_open(path, &fridge_db) == SQLITE_OK);
    assert(sqlite3_exec(fridge_db,
        "CREATE TABLE r2_fridge_items ("
        "name TEXT PRIMARY KEY COLLATE NOCASE,"
        "qty INTEGER NOT NULL,"
        "amount INTEGER NOT NULL,"
        "description TEXT);"
        "INSERT INTO r2_fridge_items(name,qty,amount,description) "
        "VALUES('legacy apples',2,99,'old inventory');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(fridge_migrate_schema() == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(fridge_db,
        "SELECT quantity,qty,amount FROM r2_fridge_items WHERE name='legacy apples'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 2);
    assert(sqlite3_column_int(st, 1) == 2);
    assert(sqlite3_column_int(st, 2) == 2);
    sqlite3_finalize(st);

    assert(fridge_insert_item_compat("new apples", "fresh stock", 3,
        25.0, 5.0, "apple", "sweet", 1) == SQLITE_DONE);
    assert(fridge_insert_item_compat("new apples", "fresh stock", 2,
        25.0, 5.0, "apple", "sweet", 1) == SQLITE_DONE);

    assert(sqlite3_prepare_v2(fridge_db,
        "SELECT quantity,qty,amount FROM r2_fridge_items WHERE name='new apples'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 5);
    assert(sqlite3_column_int(st, 1) == 5);
    assert(sqlite3_column_int(st, 2) == 5);
    sqlite3_finalize(st);

    int previous_quantity = 0;
    assert(fridge_change_one("new apples", NULL, 0, &previous_quantity,
        NULL, NULL, NULL, 0, NULL, 0) == 0);
    assert(previous_quantity == 5);
    assert(sqlite3_prepare_v2(fridge_db,
        "SELECT quantity,qty,amount FROM r2_fridge_items WHERE name='new apples'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 4);
    assert(sqlite3_column_int(st, 1) == 4);
    assert(sqlite3_column_int(st, 2) == 4);
    sqlite3_finalize(st);

    assert(sqlite3_close(fridge_db) == SQLITE_OK);
    fridge_db = NULL;
    assert(unlink(path) == 0);
    puts("Legacy fridge schema migration preserves old stock and supports add, update, and take writes.");
    return 0;
}
