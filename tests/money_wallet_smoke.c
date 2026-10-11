#define _GNU_SOURCE
#include "Reality.h"
#include <sqlite3.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef R2_ROOT
#error R2_ROOT must be set to the isolated test directory
#endif
static int bill_count(const char *dir) {
    DIR *dp=opendir(dir); assert(dp);
    int count=0; struct dirent *e;
    while ((e=readdir(dp))) {
        if (!strcmp(e->d_name,"money")) { ++count; continue; }
        size_t n=strlen(e->d_name);
        if (n<8 || strncmp(e->d_name,"money(",6) || e->d_name[n-1]!=')') continue;
        int digits=1;
        for (size_t i=6;i+1<n;++i) if (e->d_name[i]<'0'||e->d_name[i]>'9') digits=0;
        if (digits) ++count;
    }
    closedir(dp); return count;
}
static int change_cents(const char *dir) {
    char path[1024]; snprintf(path,sizeof(path),"%s/change.txt",dir);
    FILE *fp=fopen(path,"r"); if(!fp)return 0;
    int n=0; assert(fscanf(fp,"cents=%d",&n)==1); fclose(fp); return n;
}
static char *money_context(void) {
    char *s=r2_reality_money_context(); assert(s); return s;
}
static void clear_money_files(const char *dir) {
    DIR *dp=opendir(dir); assert(dp); struct dirent *e;
    while ((e=readdir(dp))) {
        size_t n=strlen(e->d_name);
        int bill=!strcmp(e->d_name,"money");
        if (!bill && n>=8 && !strncmp(e->d_name,"money(",6) && e->d_name[n-1]==')') {
            bill=1;
            for(size_t i=6;i+1<n;++i)
                if(e->d_name[i]<'0'||e->d_name[i]>'9') bill=0;
        }
        if (!bill && strcmp(e->d_name,"change.txt")) continue;
        char path[1400]; struct stat st;
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
        if (lstat(path,&st)==0 && S_ISREG(st.st_mode)) assert(unlink(path)==0);
    }
    closedir(dp);
}

static void create_legacy_reality_and_fridge_databases(void)
{
    char home[1200], path[1500];
    snprintf(home, sizeof(home), "%s/R2", R2_ROOT);
    assert(mkdir(home, 0755) == 0);

    snprintf(path, sizeof(path), "%s/r2_reality.db", home);
    sqlite3 *legacy = NULL;
    assert(sqlite3_open(path, &legacy) == SQLITE_OK);
    char *error = NULL;
    int rc = sqlite3_exec(legacy,
        "CREATE TABLE r2_reality_self ("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "hunger REAL NOT NULL DEFAULT 0,"
        "last_tick INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO r2_reality_self(id,hunger,last_tick) "
        "VALUES(1,17.5,strftime('%s','now'));",
        NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could not create legacy Reality fixture: %s\n",
                error ? error : sqlite3_errmsg(legacy));
        sqlite3_free(error);
        sqlite3_close(legacy);
        abort();
    }
    sqlite3_close(legacy);

    snprintf(path, sizeof(path), "%s/r2_fridge.db", home);
    legacy = NULL;
    assert(sqlite3_open(path, &legacy) == SQLITE_OK);
    error = NULL;
    rc = sqlite3_exec(legacy,
        "CREATE TABLE r2_fridge_items ("
        "name TEXT PRIMARY KEY COLLATE NOCASE,"
        "description TEXT,"
        "qty INTEGER);"
        "INSERT INTO r2_fridge_items(name,description,qty) "
        "VALUES('legacy snack','Pre-existing user stock',3);",
        NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Could not create legacy fridge fixture: %s\n",
                error ? error : sqlite3_errmsg(legacy));
        sqlite3_free(error);
        sqlite3_close(legacy);
        abort();
    }
    sqlite3_close(legacy);
}

static void assert_legacy_reality_and_fridge_preserved(void)
{
    char path[1500];
    snprintf(path, sizeof(path), "%s/R2/r2_reality.db", R2_ROOT);
    sqlite3 *check = NULL;
    assert(sqlite3_open(path, &check) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(check,
        "SELECT hunger,satisfaction,seconds_since_meal,sleepiness,energy,last_tick,updated_at "
        "FROM r2_reality_self WHERE id=1", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    double hunger = sqlite3_column_double(st, 0);
    assert(hunger >= 82.4 && hunger <= 82.6); /* legacy hunger 17.5 -> fullness 82.5 */
    assert(sqlite3_column_double(st, 1) >= 0.0);
    assert(sqlite3_column_double(st, 2) >= 0.0);
    assert(sqlite3_column_double(st, 3) >= 0.0);
    assert(sqlite3_column_double(st, 4) >= 0.0);
    assert(sqlite3_column_int64(st, 5) > 0);
    const unsigned char *updated = sqlite3_column_text(st, 6);
    assert(updated && *updated);
    sqlite3_finalize(st);
    sqlite3_close(check);

    snprintf(path, sizeof(path), "%s/R2/r2_fridge.db", R2_ROOT);
    assert(sqlite3_open(path, &check) == SQLITE_OK);
    assert(sqlite3_prepare_v2(check,
        "SELECT quantity,fullness,energy,updated_at FROM r2_fridge_items "
        "WHERE name='legacy snack'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 3);
    /* Legacy stock without nutrition columns stays inventory, but its
       nutritional values remain explicitly unknown rather than fabricated. */
    assert(sqlite3_column_double(st, 1) == 0.0);
    assert(sqlite3_column_double(st, 2) == 0.0);
    updated = sqlite3_column_text(st, 3);
    assert(updated && *updated);
    sqlite3_finalize(st);
    sqlite3_close(check);
}
int main(void) {
    char wallet[1024],bank[1024],bill[1200];
    snprintf(wallet,sizeof(wallet),"%s/Pockets/Wallet",R2_ROOT);
    snprintf(bank,sizeof(bank),"%s/room/piggybank",R2_ROOT);
    assert(mkdir(R2_ROOT,0755)==0);
    char room_root[1200]; snprintf(room_root,sizeof(room_root),"%s/room",R2_ROOT);
    assert(mkdir(room_root,0755)==0);
    /* Upgrade old lowercase and room-local mirror folders without losing portable items. */
    char legacy[1400], legacy_wallet[1500], legacy_room_pockets[1500], legacy_room_wallet[1500], migrated[2048];
    snprintf(legacy_room_pockets,sizeof(legacy_room_pockets),"%s/room/pockets",R2_ROOT);
    snprintf(legacy_room_wallet,sizeof(legacy_room_wallet),"%s/room/wallet",R2_ROOT);
    assert(mkdir(legacy_room_pockets,0755)==0);
    assert(mkdir(legacy_room_wallet,0755)==0);
    snprintf(migrated,sizeof(migrated),"%s/legacy_room_item.r2item",legacy_room_pockets);
    FILE *lf=fopen(migrated,"w"); assert(lf); fputs("old room pocket item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/legacy_room_wallet_item.r2item",legacy_room_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("old room wallet item\n",lf); fclose(lf);
    snprintf(legacy,sizeof(legacy),"%s/pockets",R2_ROOT);
    snprintf(legacy_wallet,sizeof(legacy_wallet),"%s/wallet",legacy);
    assert(mkdir(legacy,0755)==0);
    assert(mkdir(legacy_wallet,0755)==0);
    snprintf(migrated,sizeof(migrated),"%s/legacy_item.r2item",legacy);
    lf=fopen(migrated,"w"); assert(lf); fputs("legacy pocket item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/legacy_wallet_item.r2item",legacy_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("legacy wallet item\n",lf); fclose(lf);
    snprintf(migrated,sizeof(migrated),"%s/cash.txt",legacy_wallet);
    lf=fopen(migrated,"w"); assert(lf); fputs("cash=999.00\n",lf); fclose(lf);
    create_legacy_reality_and_fridge_databases();
    assert(r2_reality_init()==0);
    assert_legacy_reality_and_fridge_preserved();
    /* The CRT is persistent room furnishing; Reality, not the GUI, owns its state. */
    char *room_items=r2_reality_list("room");
    assert(room_items && strstr(room_items,"TV"));
    free(room_items);
    char *tv_state=r2_reality_tv_status();
    assert(tv_state && strstr(tv_state,"CRT TV: OFF") && strstr(tv_state,"NO SIGNAL"));
    free(tv_state);
    assert(r2_reality_tv_power(1)==0);
    assert(r2_reality_tv_connect("Atari 2600","rf",3)==0);
    assert(r2_reality_tv_tune_rf(3)==0);
    tv_state=r2_reality_tv_status();
    assert(tv_state && strstr(tv_state,"Atari 2600") && strstr(tv_state,"signal: available"));
    free(tv_state);
    assert(r2_reality_tv_disconnect("Atari 2600")==0);
    tv_state=r2_reality_tv_status();
    assert(tv_state && strstr(tv_state,"NO SIGNAL"));
    free(tv_state);
    assert(r2_reality_tv_power(0)==0);
    assert(access(migrated,F_OK)!=0); /* The old cash.txt was only a stale derived summary. */
    assert(access(legacy,F_OK)!=0); /* Empty duplicate lowercase pocket folders are removed. */
    snprintf(migrated,sizeof(migrated),"%s/Pockets/legacy_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/Wallet/legacy_wallet_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/legacy_room_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    snprintf(migrated,sizeof(migrated),"%s/Pockets/Wallet/legacy_room_wallet_item.r2item",R2_ROOT);
    assert(access(migrated,F_OK)==0);
    char *ctx=NULL;
    assert(bill_count(wallet)==5);
    /* An unexpected non-regular authoritative path must be rejected before
       any valid cash bills are removed or recreated. */
    char bad_money_path[1400];
    snprintf(bad_money_path, sizeof(bad_money_path), "%s/money(999)", wallet);
    assert(symlink("/dev/null", bad_money_path) == 0);
    /* A preflight rejection must preserve cash without falsely reporting a
       rollback failure: no authoritative file was changed, so no rollback
       should be attempted. */
    FILE *captured_stderr = tmpfile();
    assert(captured_stderr);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(fflush(stderr) == 0);
    assert(dup2(fileno(captured_stderr), STDERR_FILENO) >= 0);
    assert(r2_reality_money_receive(1.00) != 0);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    rewind(captured_stderr);
    char captured_message[2048] = {0};
    size_t captured_len = fread(captured_message, 1, sizeof(captured_message) - 1, captured_stderr);
    assert(captured_len < sizeof(captured_message));
    captured_message[captured_len] = '\0';
    fclose(captured_stderr);
    assert(strstr(captured_message, "CRITICAL: cash receipt failed") == NULL);
    assert(unlink(bad_money_path) == 0);
    assert(bill_count(wallet) == 5);
    ctx=money_context(); assert(strstr(ctx,"carried cash=$5.00")); free(ctx);
    assert(r2_reality_money_receive(1000000.01)!=0); /* Every money operation has the same hard cap. */
    assert(r2_reality_money_receive(1000000.00)!=0); /* A wallet cannot exceed its $1M balance cap. */
    assert(bill_count(wallet)==5);
    ctx=money_context(); assert(strstr(ctx,"carried cash=$5.00")); free(ctx);

    snprintf(bill,sizeof(bill),"%s/money",wallet);
    assert(unlink(bill)==0);
    ctx=money_context(); assert(strstr(ctx,"carried cash=$4.00")); free(ctx);
    r2_reality_shutdown();
    assert(r2_reality_init()==0);
    assert(bill_count(wallet)==4); /* Restart must not refill deleted money. */

    assert(r2_reality_money_deposit(1.50)==0);
    assert(bill_count(wallet)==2 && change_cents(wallet)==50);
    assert(bill_count(bank)==1 && change_cents(bank)==50);
    assert(r2_reality_money_withdraw(0.50)==0);
    assert(bill_count(wallet)==3 && change_cents(wallet)==0);
    assert(bill_count(bank)==1 && change_cents(bank)==0);
    /* A food purchase destined for the fridge must use an explicit metric,
       just like any other fridge addition; the test supplies its own fixture. */
    char metrics_path[1400];
    snprintf(metrics_path, sizeof(metrics_path), "%s/room/food_metrics.xml", R2_ROOT);
    FILE *metrics = fopen(metrics_path, "w");
    assert(metrics);
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<foods>\n"
          "  <food name=\"soda\" fullness=\"25\" energy=\"3\" ingredients=\"carbonated water,sugar\" taste=\"sweet,fizzy\" />\n"
          "</foods>\n", metrics);
    assert(fclose(metrics) == 0);
    assert(r2_reality_buy_item("soda","A purchased drink",3.50,"fridge")==0);
    /* Pockets travel with R2; room storage and the fixed fridge do not. */
    assert(r2_reality_set_location(0)==0);
    assert(r2_reality_add_item("travel toy","Portable toy","pockets",1)==0);
    char pocket_item[1200];
    snprintf(pocket_item,sizeof(pocket_item),"%s/Pockets/travel_toy.r2item",R2_ROOT);
    assert(access(pocket_item,F_OK)==0); /* Item mirrors share the authoritative Pockets tree. */
    assert(r2_reality_add_item("room-only toy","Toy left at home","room",1)!=0);
    assert(r2_fridge_take("soda")!=0);
    assert(r2_reality_set_location(1)==0);
    assert(bill_count(wallet)==0 && change_cents(wallet)==0);
    assert(bill_count(bank)==0 && change_cents(bank)==50);
    assert(r2_reality_buy_item("expensive item","Not affordable",1.00,"pockets")!=0);
    ctx=money_context(); assert(strstr(ctx,"total=$0.50")); free(ctx);
    r2_reality_shutdown();
    assert(r2_reality_init()==0);
    assert(bill_count(wallet)==0 && bill_count(bank)==0);
    assert(change_cents(bank)==50); /* Fractional change persists without a seed. */
    snprintf(bill,sizeof(bill),"%s/change.txt",bank);
    assert(unlink(bill)==0);
    ctx=money_context(); assert(strstr(ctx,"total=$0.00")); free(ctx);

    /* If all physical cash files disappear while R2 is stopped, the stale
       SQLite summary must not repopulate them on the next startup. */
    assert(r2_reality_money_receive(2.00)==0);
    r2_reality_shutdown();
    clear_money_files(wallet);
    clear_money_files(bank);
    assert(r2_reality_init()==0);
    assert(bill_count(wallet)==0 && change_cents(wallet)==0);
    assert(bill_count(bank)==0 && change_cents(bank)==0);
    ctx=money_context(); assert(strstr(ctx,"total=$0.00")); free(ctx);
    r2_reality_shutdown();
    puts("Physical money wallet and CRT TV Reality smoke tests passed.");
    return 0;
}
