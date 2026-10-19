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
#include <limits.h>
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
/* Serializes a purchase from durable intent through inventory commit, including all other money mutations. */
static pthread_mutex_t purchase_lock = PTHREAD_MUTEX_INITIALIZER;
static int reality_ready = 0;
static int reality_eat_internal(const char *food, double fullness, int consume_tracked_item, double energy_override);
static int purchase_journal_recover(void);

/* The hunger field is a 0..100 fullness meter: it drains to zero over three
 * modeled hours without food and rises when R2 eats. The separate 72-hour
 * starvation marker tracks elapsed time since a qualifying meal. Sleepiness
 * rises by 12.5/hour and energy falls by 10/hour; sleep recovery reverses those rates. */
static const double HUNGER_PER_SECOND = 100.0 / (3.0 * 3600.0);
static const double SATIETY_PER_SECOND = 0.1 / 4.75;
static const double SLEEPINESS_PER_SECOND = 12.5 / 3600.0;
static const double ENERGY_DRAIN_PER_SECOND = 10.0 / 3600.0;

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
/* Physical money files are authoritative. Each money/money(N) file is $1;
 * change.txt stores cents below $1. SQLite/account.txt are derived summaries.
 * Reads never create missing money files, so deleted/spent cash stays gone. */
static int money_name_is_bill(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "money")) return 1;
    size_t n = strlen(name);
    if (n < 8 || strncmp(name, "money(", 6) || name[n-1] != ')') return 0;
    for (size_t i=6; i+1<n; ++i)
        if (!isdigit((unsigned char)name[i])) return 0;
    return 1;
}
static void money_dir_path(int bank, char *out, size_t cap)
{
    if (bank) snprintf(out, cap, "%s/room/piggybank", R2_ROOT);
    else snprintf(out, cap, "%s/Pockets/Wallet", R2_ROOT);
}
static int money_bill_count(const char *dir)
{
    DIR *dp=opendir(dir);
    if (!dp) return -1;
    int count=0; struct dirent *entry;
    while ((entry=readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name)) continue;
        char path[2048]; struct stat st;
        int n=snprintf(path,sizeof(path),"%s/%s",dir,entry->d_name);
        if (n>0 && (size_t)n<sizeof(path) && lstat(path,&st)==0 && S_ISREG(st.st_mode)) ++count;
    }
    closedir(dp);
    return count;
}
static int money_change_cents(const char *dir)
{
    char path[2048]; struct stat st;
    int n=snprintf(path,sizeof(path),"%s/change.txt",dir);
    if (n<=0 || (size_t)n>=sizeof(path)) return -1;
    if (lstat(path,&st)!=0) return errno==ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode)) return -1;
    FILE *fp=fopen(path,"r");
    if (!fp) return -1;
    long cents=-1; int ok=fscanf(fp,"cents=%ld",&cents)==1; fclose(fp);
    return ok && cents>=0 && cents<100 ? (int)cents : -1;
}
static int money_create_bill(const char *dir)
{
    char path[2048], name[64];
    for (unsigned long suffix=0; suffix<1000000UL; ++suffix) {
        if (!suffix) snprintf(name,sizeof(name),"money");
        else snprintf(name,sizeof(name),"money(%lu)",suffix);
        int n=snprintf(path,sizeof(path),"%s/%s",dir,name);
        if (n<=0 || (size_t)n>=sizeof(path)) return -1;
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (fd>=0) {
            static const char note[]="denomination=1.00\n";
            size_t offset = 0;
            int failed = 0;
            while (offset < sizeof(note) - 1) {
                ssize_t wrote = write(fd, note + offset, sizeof(note) - 1 - offset);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) { failed = 1; break; }
                offset += (size_t)wrote;
            }
            if (!failed && fsync(fd) != 0) failed = 1;
            if (close(fd) != 0) failed = 1;
            if (failed) { (void)unlink(path); return -1; }
            return 0;
        }
        if (errno!=EEXIST) return -1;
    }
    return -1;
}

static int money_sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc == 0 ? 0 : -1;
}

static int money_write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static int money_txn_path(char *out, size_t cap, const char *txn, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", txn, leaf);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int money_write_phase(const char *txn, const char *state)
{
    char phase[4096], temp[4096];
    if (money_txn_path(phase, sizeof(phase), txn, "phase") != 0 ||
        money_txn_path(temp, sizeof(temp), txn, ".phase.tmp") != 0) return -1;
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t length = strlen(state);
    int rc = money_write_all(fd, state, length);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && rename(temp, phase) != 0) rc = -1;
    if (rc == 0 && money_sync_directory(txn) != 0) rc = -1;
    if (rc != 0) (void)unlink(temp);
    return rc;
}

static int money_remove_auth_files(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) continue;
        char path[8192];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (n <= 0 || (size_t)n >= sizeof(path) ||
            lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || unlink(path) != 0) {
            rc = -1;
            break;
        }
    }
    if (closedir(dp) != 0) rc = -1;
    return rc;
}

static int money_restore_backup(const char *dir, const char *backup)
{
    DIR *dp = opendir(backup);
    if (!dp) return -1;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) continue;
        char from[8192], to[8192];
        struct stat st;
        int a = snprintf(from, sizeof(from), "%s/%s", backup, entry->d_name);
        int b = snprintf(to, sizeof(to), "%s/%s", dir, entry->d_name);
        if (a <= 0 || (size_t)a >= sizeof(from) || b <= 0 || (size_t)b >= sizeof(to) ||
            lstat(from, &st) != 0 || !S_ISREG(st.st_mode)) {
            rc = -1;
            break;
        }
        if (lstat(to, &st) == 0 || errno != ENOENT || rename(from, to) != 0) {
            rc = -1;
            break;
        }
    }
    if (closedir(dp) != 0) rc = -1;
    if (rc == 0 && money_sync_directory(backup) != 0) rc = -1;
    if (rc == 0 && money_sync_directory(dir) != 0) rc = -1;
    return rc;
}

static int money_clean_txn_dir(const char *txn)
{
    char stage[4096], backup[4096], phase[4096], temp[4096];
    if (money_txn_path(stage, sizeof(stage), txn, "new") != 0 ||
        money_txn_path(backup, sizeof(backup), txn, "backup") != 0 ||
        money_txn_path(phase, sizeof(phase), txn, "phase") != 0 ||
        money_txn_path(temp, sizeof(temp), txn, ".phase.tmp") != 0) return -1;
    const char *dirs[] = {stage, backup};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        DIR *dp = opendir(dirs[i]);
        if (!dp) {
            if (errno == ENOENT) continue;
            return -1;
        }
        struct dirent *entry;
        int rc = 0;
        while ((entry = readdir(dp)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) {
                rc = -1;
                break;
            }
            char path[8192];
            struct stat st;
            int n = snprintf(path, sizeof(path), "%s/%s", dirs[i], entry->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path) || lstat(path, &st) != 0 ||
                !S_ISREG(st.st_mode) || unlink(path) != 0) {
                rc = -1;
                break;
            }
        }
        if (closedir(dp) != 0) rc = -1;
        if (rc != 0) return -1;
        if (rmdir(dirs[i]) != 0 && errno != ENOENT) return -1;
    }
    if (unlink(phase) != 0 && errno != ENOENT) return -1;
    if (unlink(temp) != 0 && errno != ENOENT) return -1;
    if (money_sync_directory(txn) != 0) return -1;
    if (rmdir(txn) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int money_recover_one_transaction(const char *dir, const char *txn)
{
    char stage[4096], backup[4096], phase[4096], state[32] = {0};
    if (money_txn_path(stage, sizeof(stage), txn, "new") != 0 ||
        money_txn_path(backup, sizeof(backup), txn, "backup") != 0 ||
        money_txn_path(phase, sizeof(phase), txn, "phase") != 0) return -1;

    int fd = open(phase, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno != ENOENT) return -1;
    if (fd >= 0) {
        ssize_t n;
        do { n = read(fd, state, sizeof(state) - 1); } while (n < 0 && errno == EINTR);
        int close_rc = close(fd);
        if (n <= 0 || close_rc != 0) return -1;
        if (n == (ssize_t)(sizeof(state) - 1)) return -1;
        state[n] = '\0';
        state[strcspn(state, "\r\n")] = '\0';
    } else {
        /* No phase marker is safe only before any authoritative file moved. */
        DIR *dp = opendir(backup);
        if (!dp) return -1;
        struct dirent *entry;
        int has_backup = 0;
        while ((entry = readdir(dp)) != NULL) {
            if (money_name_is_bill(entry->d_name) || !strcmp(entry->d_name, "change.txt")) {
                has_backup = 1;
                break;
            }
        }
        if (closedir(dp) != 0 || has_backup) return -1;
        return money_clean_txn_dir(txn);
    }

    if (!strcmp(state, "moving")) {
        /* Some old bills may still be in place; restore only names already backed up. */
        if (money_restore_backup(dir, backup) != 0) return -1;
    } else if (!strcmp(state, "installing")) {
        if (money_remove_auth_files(dir) != 0 ||
            money_restore_backup(dir, backup) != 0) return -1;
    } else if (strcmp(state, "committed")) {
        /* Unknown/truncated state is not permission to discard the backup. */
        return -1;
    }
    if (money_sync_directory(dir) != 0) return -1;
    return money_clean_txn_dir(txn);
}

static int money_recover_transactions(const char *dir)
{
    DIR *root = opendir(dir);
    if (!root) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(root)) != NULL) {
        if (strncmp(entry->d_name, ".r2-money-txn.", 14)) continue;
        char txn[4096];
        struct stat st;
        int n = snprintf(txn, sizeof(txn), "%s/%s", dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(txn) ||
            lstat(txn, &st) != 0 || !S_ISDIR(st.st_mode) ||
            money_recover_one_transaction(dir, txn) != 0) {
            result = -1;
            break;
        }
    }
    if (closedir(root) != 0) result = -1;
    return result;
}

static int money_set_dir_balance(const char *dir, sqlite3_int64 cents, int *changed)
{
    if (changed) *changed = 0;
    if (cents < 0 || cents > 100000000LL || ensure_dir_tree(dir) != 0 ||
        money_recover_transactions(dir) != 0) return -1;

    DIR *dp = opendir(dir);
    if (!dp) return -1;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) continue;
        char path[4096];
        struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path) ||
            lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) { rc = -1; break; }
    }
    if (closedir(dp) != 0) rc = -1;
    if (rc) return -1;

    char templ[2200], txn[4096], stage[4096], backup[4096];
    int n = snprintf(templ, sizeof(templ), "%s/.r2-money-txn.XXXXXX", dir);
    if (n <= 0 || (size_t)n >= sizeof(templ)) return -1;
    char *made = mkdtemp(templ);
    if (!made) return -1;
    snprintf(txn, sizeof(txn), "%s", made);
    if (money_sync_directory(dir) != 0 ||
        money_txn_path(stage, sizeof(stage), txn, "new") != 0 ||
        money_txn_path(backup, sizeof(backup), txn, "backup") != 0 ||
        mkdir(stage, 0700) != 0 || mkdir(backup, 0700) != 0 ||
        money_sync_directory(txn) != 0) {
        (void)money_clean_txn_dir(txn);
        return -1;
    }

    for (sqlite3_int64 i = 0; i < cents / 100; ++i) {
        if (money_create_bill(stage) != 0) { rc = -1; break; }
#ifdef R2_MONEY_TEST_FAILURE_INJECTION
        if (i == 0 && getenv("R2_TEST_MONEY_STAGE_FAIL")) { rc = -1; break; }
#endif
    }
    int change = (int)(cents % 100);
    if (!rc && change) {
        char path[4096], value[32];
        n = snprintf(path, sizeof(path), "%s/change.txt", stage);
        if (n <= 0 || (size_t)n >= sizeof(path)) rc = -1;
        else {
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) rc = -1;
            else {
                int length = snprintf(value, sizeof(value), "cents=%d\n", change);
                if (money_write_all(fd, value, (size_t)length) != 0 || fsync(fd) != 0) rc = -1;
                if (close(fd) != 0) rc = -1;
                if (rc) (void)unlink(path);
            }
        }
    }
    if (rc == 0 && money_sync_directory(stage) != 0) rc = -1;
    if (rc != 0) {
        (void)money_clean_txn_dir(txn);
        return -1;
    }

    if (money_write_phase(txn, "moving\n") != 0) {
        (void)money_clean_txn_dir(txn);
        return -1;
    }
    dp = opendir(dir);
    if (!dp) { rc = -1; goto rollback; }
    while ((entry = readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) continue;
        char from[8192], to[8192];
        int a = snprintf(from, sizeof(from), "%s/%s", dir, entry->d_name);
        int b = snprintf(to, sizeof(to), "%s/%s", backup, entry->d_name);
        if (a <= 0 || (size_t)a >= sizeof(from) || b <= 0 ||
            (size_t)b >= sizeof(to) || rename(from, to) != 0) { rc = -1; break; }
#ifdef R2_MONEY_TEST_FAILURE_INJECTION
        if (getenv("R2_TEST_MONEY_FAIL_AFTER_BACKUP_MOVE")) { rc = -1; break; }
#endif
    }
    if (closedir(dp) != 0) rc = -1;
    if (rc != 0 || money_sync_directory(dir) != 0 || money_sync_directory(backup) != 0) {
        rc = -1;
        goto rollback;
    }

    if (money_write_phase(txn, "installing\n") != 0) { rc = -1; goto rollback; }
    dp = opendir(stage);
    if (!dp) { rc = -1; goto rollback; }
    while ((entry = readdir(dp)) != NULL) {
        if (!money_name_is_bill(entry->d_name) && strcmp(entry->d_name, "change.txt")) continue;
        char from[8192], to[8192];
        int a = snprintf(from, sizeof(from), "%s/%s", stage, entry->d_name);
        int b = snprintf(to, sizeof(to), "%s/%s", dir, entry->d_name);
        if (a <= 0 || (size_t)a >= sizeof(from) || b <= 0 ||
            (size_t)b >= sizeof(to) || rename(from, to) != 0) { rc = -1; break; }
    }
    if (closedir(dp) != 0) rc = -1;
    if (rc != 0 || money_sync_directory(dir) != 0 || money_sync_directory(stage) != 0) {
        rc = -1;
        goto rollback;
    }

    if (money_write_phase(txn, "committed\n") != 0) { rc = -1; goto rollback; }
    if (changed) *changed = 1;
    (void)money_recover_one_transaction(dir, txn); /* committed cleanup can be retried at startup */
    return 0;

rollback:
    /* Never delete a backup unless restoration completed successfully. */
    if (money_recover_one_transaction(dir, txn) != 0)
        fprintf(stderr, "[R2 Reality] Money transaction recovery deferred; backup retained at %s\n", txn);
    return -1;
}

static int money_dir_balance(const char *dir, sqlite3_int64 *balance)
{
    int bills=money_bill_count(dir), change=money_change_cents(dir);
    if (bills<0 || change<0) return -1;
    if (balance) *balance=(sqlite3_int64)bills*100+change;
    return 0;
}
static void money_mirror_write(sqlite3_int64 cash, sqlite3_int64 bank)
{
    char dir[1200], path[1400], tmp[1500], legacy_cash[1500];
    money_dir_path(1,dir,sizeof(dir));
    if (ensure_dir_tree(dir)!=0) return;
    snprintf(path,sizeof(path),"%s/account.txt",dir);
    snprintf(tmp,sizeof(tmp),"%s.tmp.%ld",path,(long)getpid());
    FILE *fp=fopen(tmp,"w"); if (!fp) return;
    int bad=fprintf(fp,"cash=%.2f\nbank=%.2f\ntotal=%.2f\nDerived summary only; physical money files are authoritative.\n",
        cash/100.0,bank/100.0,(cash+bank)/100.0)<0;
    if (fclose(fp)!=0) bad=1;
    if (!bad && rename(tmp,path)!=0) bad=1;
    if (bad) (void)unlink(tmp);
    char wallet[1200], old_cash[1400];
    money_dir_path(0,wallet,sizeof(wallet));
    snprintf(old_cash,sizeof(old_cash),"%s/cash.txt",wallet);
    (void)unlink(old_cash);
    snprintf(legacy_cash,sizeof(legacy_cash),"%s/pockets/wallet/cash.txt",R2_ROOT);
    (void)unlink(legacy_cash);
    snprintf(legacy_cash,sizeof(legacy_cash),"%s/pockets/wallet",R2_ROOT);
    (void)rmdir(legacy_cash);
    snprintf(legacy_cash,sizeof(legacy_cash),"%s/pockets",R2_ROOT);
    (void)rmdir(legacy_cash);
}
static int money_read_locked(sqlite3_int64 *cash, sqlite3_int64 *bank)
{
    char wallet[1200], piggybank[1200];
    sqlite3_int64 c=0,b=0;
    money_dir_path(0,wallet,sizeof(wallet));
    money_dir_path(1,piggybank,sizeof(piggybank));
    if (money_dir_balance(wallet,&c)!=0 || money_dir_balance(piggybank,&b)!=0) return -1;
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(reality_db,
        "UPDATE r2_money_account SET cash_cents=?,bank_cents=?,updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1,&st,NULL);
    if (rc==SQLITE_OK) { sqlite3_bind_int64(st,1,c); sqlite3_bind_int64(st,2,b); rc=sqlite3_step(st); }
    if (st) sqlite3_finalize(st);
    if (rc!=SQLITE_DONE) return -1;
    if (cash) *cash=c;
    if (bank) *bank=b;
    return 0;
}
static int money_journal_clear(void)
{
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE; DELETE FROM r2_money_transfer_journal WHERE id=1; COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int money_journal_begin(sqlite3_int64 old_cash, sqlite3_int64 old_bank,
                               sqlite3_int64 new_cash, sqlite3_int64 new_bank,
                               const char *operation)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_money_transfer_journal(id,old_cash_cents,old_bank_cents,new_cash_cents,new_bank_cents,operation,phase) "
        "VALUES(1,?,?,?,?,?,'pending')", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, old_cash);
        sqlite3_bind_int64(st, 2, old_bank);
        sqlite3_bind_int64(st, 3, new_cash);
        sqlite3_bind_int64(st, 4, new_bank);
        sqlite3_bind_text(st, 5, operation ? operation : "transfer", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int money_journal_mark_committed(void)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_money_transfer_journal SET phase='committed' WHERE id=1 AND phase='pending'",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 1)
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    else rc = SQLITE_ERROR;
    if (rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

/* Resolve a durable two-directory transfer intent before trusting balances. */
static int money_journal_recover(int *was_committed)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 old_cash = 0, old_bank = 0, new_cash = 0, new_bank = 0;
    char phase[24] = {0};
    if (was_committed) *was_committed = 0;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT old_cash_cents,old_bank_cents,new_cash_cents,new_bank_cents,phase "
        "FROM r2_money_transfer_journal WHERE id=1", -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { sqlite3_finalize(st); return 0; }
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    old_cash = sqlite3_column_int64(st, 0);
    old_bank = sqlite3_column_int64(st, 1);
    new_cash = sqlite3_column_int64(st, 2);
    new_bank = sqlite3_column_int64(st, 3);
    const unsigned char *p = sqlite3_column_text(st, 4);
    snprintf(phase, sizeof(phase), "%s", p ? (const char *)p : "");
    sqlite3_finalize(st);

    if (!strcmp(phase, "pending")) {
        char wallet[1200], piggybank[1200];
        money_dir_path(0, wallet, sizeof(wallet));
        money_dir_path(1, piggybank, sizeof(piggybank));
        if (money_set_dir_balance(wallet, old_cash, NULL) != 0 ||
            money_set_dir_balance(piggybank, old_bank, NULL) != 0 ||
            money_read_locked(NULL, NULL) != 0) {
            fprintf(stderr, "[R2 Money] Pending transfer recovery failed; durable journal retained for retry.\n");
            return -1;
        }
        return money_journal_clear();
    }
    if (strcmp(phase, "committed")) return -1;
    if (was_committed) *was_committed = 1;
    /* Physical files remain authoritative; this refreshes the derived SQLite summary. */
    if (money_read_locked(NULL, NULL) != 0) return -1;
    (void)new_cash;
    (void)new_bank;
    return money_journal_clear();
}

static int money_initialize_files(int previously_seeded)
{
    char wallet[1200], piggybank[1200];
    money_dir_path(0, wallet, sizeof(wallet));
    money_dir_path(1, piggybank, sizeof(piggybank));
    if (ensure_dir_tree(wallet) != 0 || ensure_dir_tree(piggybank) != 0) return -1;
    if (money_journal_recover(NULL) != 0) return -1;
    if (money_recover_transactions(wallet) != 0 || money_recover_transactions(piggybank) != 0) return -1;

    int wb = money_bill_count(wallet), bb = money_bill_count(piggybank);
    int wc = money_change_cents(wallet), bc = money_change_cents(piggybank);
    if (wb < 0 || bb < 0 || wc < 0 || bc < 0) return -1;

    int physical_authoritative = 0;
    sqlite3_stmt *meta = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT value FROM r2_reality_meta WHERE key='money_files_authoritative'",
        -1, &meta, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(meta);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(meta, 0);
            physical_authoritative = value && !strcmp((const char *)value, "1");
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_OK;
        }
    }
    if (meta) sqlite3_finalize(meta);
    if (rc != SQLITE_OK) return -1;

    if (wb == 0 && bb == 0 && wc == 0 && bc == 0) {
        if (physical_authoritative) {
            /* Once physical files have been authoritative, an empty wallet
               remains empty. A stale SQLite summary must never respawn cash. */
        } else if (!previously_seeded) {
            if (money_set_dir_balance(wallet, 500, NULL) != 0) return -1;
        } else {
            /* First migration from a legacy database only: preserve its
               remaining balance before physical files become authoritative. */
            sqlite3_stmt *st = NULL;
            rc = sqlite3_prepare_v2(reality_db,
                "SELECT cash_cents,bank_cents FROM r2_money_account WHERE id=1",
                -1, &st, NULL);
            if (rc == SQLITE_OK) {
                rc = sqlite3_step(st);
                if (rc == SQLITE_ROW) rc = SQLITE_OK;
            }
            if (rc != SQLITE_OK) {
                if (st) sqlite3_finalize(st);
                return -1;
            }
            sqlite3_int64 cash = sqlite3_column_int64(st, 0);
            sqlite3_int64 bank = sqlite3_column_int64(st, 1);
            sqlite3_finalize(st);
            if (money_set_dir_balance(wallet, cash, NULL) != 0 ||
                money_set_dir_balance(piggybank, bank, NULL) != 0) return -1;
        }
    }
    return exec_sql(
        "INSERT INTO r2_reality_meta(key,value) VALUES('initial_cash_seeded','1') "
        "ON CONFLICT(key) DO UPDATE SET value='1';"
        "INSERT INTO r2_reality_meta(key,value) VALUES('money_files_authoritative','1') "
        "ON CONFLICT(key) DO UPDATE SET value='1';");
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
            /* A same-name destination is not proof that the old file is a
               duplicate. Preserve both copies rather than silently deleting
               potentially newer or user-relevant legacy state. The old
               directory intentionally remains if this collision persists. */
            fprintf(stderr,
                "[R2 Reality] Preserving legacy mirror %s; destination already exists at %s.\n",
                old_path, new_path);
        } else if (rename(old_path, new_path) != 0) result = -1;
    }
    closedir(dir);
    (void)rmdir(old_dir); /* Never recursively delete user-created content. */
    return result;
}

static int make_room_dirs(void)
{
    char room[1024], shelf[1100], box[1100], pockets[1100], wallet[1200];
    char fridge[1100], piggybank[1100], diary[1100];
    char old_pockets[1200], old_wallet[1200], old_room_pockets[1200], old_room_wallet[1200];
    char old_fridge[1200], old_toy_box[1200];
    snprintf(room,sizeof(room),"%s/room",R2_ROOT); snprintf(shelf,sizeof(shelf),"%s/shelf",room);
    snprintf(box,sizeof(box),"%s/box",room); snprintf(pockets,sizeof(pockets),"%s/Pockets",R2_ROOT);
    snprintf(wallet,sizeof(wallet),"%s/Wallet",pockets);
    snprintf(fridge,sizeof(fridge),"%s/fridge",R2_ROOT); snprintf(piggybank,sizeof(piggybank),"%s/piggybank",room);
    snprintf(old_pockets,sizeof(old_pockets),"%s/pockets",R2_ROOT);
    snprintf(old_wallet,sizeof(old_wallet),"%s/pockets/wallet",R2_ROOT);
    snprintf(old_room_pockets,sizeof(old_room_pockets),"%s/room/pockets",R2_ROOT);
    snprintf(old_room_wallet,sizeof(old_room_wallet),"%s/room/wallet",R2_ROOT);
    snprintf(old_fridge,sizeof(old_fridge),"%s/room/fridge",R2_ROOT);
    snprintf(old_toy_box,sizeof(old_toy_box),"%s/room/toy_box",R2_ROOT);
    snprintf(diary,sizeof(diary),"%s",R2_DIARY_DIR);
    if(ensure_dir_tree(R2_ROOT)||ensure_dir_tree(R2_HOME)||ensure_dir_tree(diary)||ensure_dir_tree(room)||
       ensure_dir_tree(shelf)||ensure_dir_tree(box)||ensure_dir_tree(pockets)||ensure_dir_tree(wallet)||
       ensure_dir_tree(fridge)||ensure_dir_tree(piggybank)) {
        fprintf(stderr, "[R2 Reality] Could not create room/shelf/box directories under %s\n", R2_ROOT);
        return -1;
    }
    if (migrate_mirror_directory(old_pockets, pockets) != 0 ||
        migrate_mirror_directory(old_wallet, wallet) != 0 ||
        migrate_mirror_directory(old_room_pockets, pockets) != 0 ||
        migrate_mirror_directory(old_room_wallet, wallet) != 0 ||
        migrate_mirror_directory(old_fridge, fridge) != 0 ||
        migrate_mirror_directory(old_toy_box, box) != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate one or more old mirror directories.\n");
        return -1;
    }
    /* Remove the obsolete lowercase parent only if it is now empty. */
    (void)rmdir(old_pockets);
    char food_xml[1200];
    snprintf(food_xml, sizeof(food_xml), "%s/food_metrics.xml", room);
    if (access(food_xml, F_OK) != 0) {
        FILE *fp = fopen(food_xml, "w");
        if (fp) {
            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<foods>\n"
                  "  <!-- Add one explicitly configured food per line. fullness is 0..100; energy is optional. -->\n"
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
 * Pockets/. SQLite remains canonical for tracked objects; mirror files are projections. */
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
    else if (!strcasecmp(container, "pockets")) snprintf(base, sizeof(base), "%s/Pockets", R2_ROOT);
    else if (!strcasecmp(container, "wallet")) snprintf(base, sizeof(base), "%s/Pockets/Wallet", R2_ROOT);
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
        /* Reject NaN/infinity explicitly: comparisons alone do not reject NaN,
           which could otherwise poison persistent need values during eating. */
        if (end == full || *end || !isfinite(f) || f < 0.0 || f > 100.0) continue;
        double e = f * 0.1;
        if (xml_attribute(line, "energy", en, sizeof(en)) == 0) {
            end = NULL;
            double parsed = strtod(en, &end);
            if (end != en && !*end && isfinite(parsed) &&
                parsed >= 0.0 && parsed <= 100.0) e = parsed;
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
        if (end == full || *end || !isfinite(f) || f < 0.0 || f > 100.0) continue;
        double e = f * 0.1;
        if (xml_attribute(line, "energy", en, sizeof(en)) == 0) {
            end = NULL;
            double parsed = strtod(en, &end);
            if (end != en && !*end && isfinite(parsed) &&
                parsed >= 0.0 && parsed <= 100.0) e = parsed;
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
    if (!len) snprintf(out, cap, "No food metrics are configured in room/food_metrics.xml.\n");
    return out;
}

static int update_hunger_locked(double elapsed)
{
    if (!reality_db || !isfinite(elapsed)) return -1;
    if (elapsed < 0.0) elapsed = 0.0;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT hunger, seconds_since_meal, sleepiness, energy, satisfaction FROM r2_reality_self WHERE id=1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    double hunger = sqlite3_column_double(st, 0) - elapsed * HUNGER_PER_SECOND;
    double since_meal = sqlite3_column_double(st, 1) + elapsed;
    double sleepiness = sqlite3_column_double(st, 2) + elapsed * SLEEPINESS_PER_SECOND;
    double energy = sqlite3_column_double(st, 3) - elapsed * ENERGY_DRAIN_PER_SECOND;
    double satisfaction = sqlite3_column_double(st, 4) - elapsed * SATIETY_PER_SECOND;
    sqlite3_finalize(st);
    if (!isfinite(hunger) || !isfinite(since_meal) || !isfinite(sleepiness) ||
        !isfinite(energy) || !isfinite(satisfaction)) return -1;
    if (hunger < 0.0) hunger = 0.0;
    if (satisfaction < 0.0) satisfaction = 0.0;
    if (satisfaction > 150.0) satisfaction = 150.0;
    if (sleepiness > 100.0) sleepiness = 100.0;
    if (energy < 0.0) energy = 0.0;
    st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=?, seconds_since_meal=?, sleepiness=?, energy=?, satisfaction=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hunger);
        sqlite3_bind_double(st, 2, since_meal);
        sqlite3_bind_double(st, 3, sleepiness);
        sqlite3_bind_double(st, 4, energy);
        sqlite3_bind_double(st, 5, satisfaction);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(reality_db) != 1) rc = SQLITE_NOTFOUND;
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_reality_meta(key,value) VALUES('world_elapsed_seconds',?) ON CONFLICT(key) DO UPDATE SET value=CAST(CAST(value AS INTEGER)+CAST(excluded.value AS INTEGER) AS TEXT)",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)llround(elapsed));
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
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
static int fridge_mirror_write(const char *name, const char *description, int quantity,
                               double fullness, double energy)
{
    char path[2048], temp[2100];
    if (fridge_mirror_path(name, path, sizeof(path)) != 0) return -1;
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(temp)) return -1;
    FILE *fp = fopen(temp, "w");
    if (!fp) return -1;
    int failed = fprintf(fp, "name=%s\ndescription=%s\nquantity=%d\ncontainer=fridge\nfullness=%.1f\nenergy=%.1f\n",
                         name, description ? description : "", quantity, fullness, energy) < 0;
    if (fclose(fp) != 0) failed = 1;
    if (!failed && rename(temp, path) != 0) failed = 1;
    if (failed) {
        (void)unlink(temp);
        return -1;
    }
    return 0;
}
static void fridge_mirror_write_warn(const char *name, const char *description,
                                     int quantity, double fullness, double energy,
                                     const char *operation)
{
    if (fridge_mirror_write(name, description, quantity, fullness, energy) != 0)
        fprintf(stderr,
                "[R2 Fridge] Database remains authoritative, but mirror update failed during %s for '%s'.\n",
                operation ? operation : "inventory update", name ? name : "(unknown)");
}

/* Empty inventory is a valid world state. Never manufacture food to
 * make the fridge appear populated: only explicit inventory additions may
 * change stock. Keep the helper so existing call sites retain their error
 * handling contract without silently creating a burger. */
static int fridge_seed_if_empty_locked(void)
{
    return fridge_db ? 0 : -1;
}
/* Remove only objects copied from the former automatic phantom-burger
 * stock. The exact legacy description is provenance; ordinary burgers and
 * all unrelated user inventory remain untouched. Caller holds reality_lock. */
static int remove_legacy_phantom_tracked_food(void)
{
    for (;;) {
        char name[REALITY_MAX_TEXT + 1] = {0};
        char container[REALITY_MAX_TEXT + 1] = {0};
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(reality_db,
            "SELECT name,container FROM r2_reality_objects "
            "WHERE description=? LIMIT 1", -1, &st, NULL);
        if (rc != SQLITE_OK) return -1;
        bind_text(st, 1,
            "Guaranteed filling burger generated because the fridge was empty");
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(st);
            return 0;
        }
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(st);
            return -1;
        }
        const unsigned char *n = sqlite3_column_text(st, 0);
        const unsigned char *c = sqlite3_column_text(st, 1);
        int name_bytes = sqlite3_column_bytes(st, 0);
        int container_bytes = sqlite3_column_bytes(st, 1);
        if (!n || !c || name_bytes <= 0 || container_bytes <= 0 ||
            (size_t)name_bytes >= sizeof(name) ||
            (size_t)container_bytes >= sizeof(container)) {
            sqlite3_finalize(st);
            return -1;
        }
        memcpy(name, n, (size_t)name_bytes);
        memcpy(container, c, (size_t)container_bytes);
        sqlite3_finalize(st);

        mirror_remove(name, container);
        st = NULL;
        rc = sqlite3_prepare_v2(reality_db,
            "DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE "
            "AND description=?", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            bind_text(st, 1, name);
            bind_text(st, 2,
                "Guaranteed filling burger generated because the fridge was empty");
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        if (rc != SQLITE_DONE || sqlite3_changes(reality_db) != 1) return -1;
    }
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
            if (name) fridge_mirror_write_warn(name, description, sqlite3_column_int(st, 2),
                sqlite3_column_double(st, 3), sqlite3_column_double(st, 4), "startup sync");
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&fridge_lock);
}

static int fridge_column_exists(const char *column)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(fridge_db, "PRAGMA table_info(r2_fridge_items)", -1,
                           &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && !strcasecmp((const char *)name, column)) {
                found = 1;
                break;
            }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

static int fridge_ensure_column(const char *column, const char *definition)
{
    if (fridge_column_exists(column)) return SQLITE_OK;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "ALTER TABLE r2_fridge_items ADD COLUMN %s %s;",
                     column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    return sqlite3_exec(fridge_db, sql, NULL, NULL, NULL);
}

/* Upgrade older fridge tables in place. Existing stock is retained; optional
   metadata is added with neutral defaults and old quantity aliases are copied
   only when the canonical quantity column did not previously exist. */
static int fridge_migrate_schema(void)
{
    int had_quantity = fridge_column_exists("quantity");
    const char *columns[][2] = {
        {"description", "TEXT"},
        {"quantity", "INTEGER NOT NULL DEFAULT 1"},
        /* Legacy rows without per-item nutrition must remain inventory, not
           gain fabricated nutritional values during migration. Unknown metrics
           use the same neutral 0/0 values as newly created fridge stock. */
        {"fullness", "REAL NOT NULL DEFAULT 0"},
        {"energy", "REAL NOT NULL DEFAULT 0"},
        {"ingredients", "TEXT"},
        {"taste", "TEXT"},
        {"updated_at", "TEXT NOT NULL DEFAULT ''"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (fridge_ensure_column(columns[i][0], columns[i][1]) != SQLITE_OK)
            return SQLITE_ERROR;
    }

    if (!had_quantity) {
        if (fridge_column_exists("qty")) {
            if (sqlite3_exec(fridge_db,
                "UPDATE r2_fridge_items SET quantity=qty WHERE qty IS NOT NULL",
                NULL, NULL, NULL) != SQLITE_OK)
                return SQLITE_ERROR;
        } else if (fridge_column_exists("amount")) {
            if (sqlite3_exec(fridge_db,
                "UPDATE r2_fridge_items SET quantity=amount WHERE amount IS NOT NULL",
                NULL, NULL, NULL) != SQLITE_OK)
                return SQLITE_ERROR;
        }
    }
    /* Canonical quantity wins once it exists. Keep any retained legacy aliases
       synchronized so older readers and required NOT NULL constraints remain
       compatible after every future stock change. */
    if (fridge_column_exists("qty") && sqlite3_exec(fridge_db,
        "UPDATE r2_fridge_items SET qty=quantity", NULL, NULL, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    if (fridge_column_exists("amount") && sqlite3_exec(fridge_db,
        "UPDATE r2_fridge_items SET amount=quantity", NULL, NULL, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    if (sqlite3_exec(fridge_db,
        "UPDATE r2_fridge_items SET updated_at=CURRENT_TIMESTAMP "
        "WHERE updated_at IS NULL OR updated_at='';",
        NULL, NULL, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    return SQLITE_OK;
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
        "CREATE TABLE IF NOT EXISTS r2_fridge_items(name TEXT PRIMARY KEY COLLATE NOCASE,description TEXT,quantity INTEGER NOT NULL CHECK(quantity>0),fullness REAL NOT NULL DEFAULT 0,energy REAL NOT NULL DEFAULT 0,ingredients TEXT,taste TEXT,updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_fridge_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT OR IGNORE INTO r2_fridge_meta(key,value) VALUES('schema_version','1');",
        NULL, NULL, NULL);
    /* Migrate older inventory layouts without deleting user-entered stock. */
    if (rc == SQLITE_OK && fridge_migrate_schema() != SQLITE_OK) rc = SQLITE_ERROR;
    /* Remove only stock created by the former automatic-refill behavior.
       Real user-entered inventory and all unrelated food are preserved. */
    if (rc == SQLITE_OK && sqlite3_exec(fridge_db,
        "DELETE FROM r2_fridge_items WHERE description='Guaranteed filling burger generated because the fridge was empty'",
        NULL, NULL, NULL) != SQLITE_OK) rc = SQLITE_ERROR;
    if (rc == SQLITE_OK && fridge_seed_if_empty_locked() != 0) rc = SQLITE_ERROR;
    pthread_mutex_unlock(&fridge_lock);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Fridge] Could not initialize fridge schema.\n");
        /* A failed init must not leave a live handle behind: Reality init
           may be retried, and shutdown must not inherit a half-open fridge. */
        pthread_mutex_lock(&fridge_lock);
        if (fridge_db) {
            (void)sqlite3_close_v2(fridge_db);
            fridge_db = NULL;
        }
        pthread_mutex_unlock(&fridge_lock);
        return -1;
    }
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
    if (!len) {
        if (rc == SQLITE_DONE)
            snprintf(out, cap, "(fridge is empty; no food is stocked)\n");
        else
            snprintf(out, cap, "(fridge inventory unavailable because the database query failed)\n");
    }
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
    if (out) snprintf(out, cap, "FRIDGE (separate persistent database; physically at home and accessible only while R2 is home):\n%sOnly listed stock exists. When the fridge is empty, it stays empty until an explicit inventory addition or storage action occurs; do not imagine or claim replacement food.", items);
    free(items);
    return out;
}
/* Older fridge databases may retain required qty/amount aliases after the
 * canonical quantity column is added. Keep those aliases populated on both
 * insert and conflict-update paths so upgrades remain writable. */
static int fridge_insert_item_compat(const char *name, const char *description,
                                     int quantity, double fullness, double energy,
                                     const char *ingredients, const char *taste,
                                     int update_metadata)
{
    const int has_qty = fridge_column_exists("qty");
    const int has_amount = fridge_column_exists("amount");
    char sql[2048];
    size_t used = 0;
    int n = snprintf(sql, sizeof(sql),
        "INSERT INTO r2_fridge_items(name,description,quantity,fullness,energy,ingredients,taste");
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    used = (size_t)n;
    if (has_qty) {
        n = snprintf(sql + used, sizeof(sql) - used, ",qty");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    if (has_amount) {
        n = snprintf(sql + used, sizeof(sql) - used, ",amount");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    n = snprintf(sql + used, sizeof(sql) - used, ") VALUES(?,?,?,?,?,?,?");
    if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
    used += (size_t)n;
    if (has_qty) {
        n = snprintf(sql + used, sizeof(sql) - used, ",?");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    if (has_amount) {
        n = snprintf(sql + used, sizeof(sql) - used, ",?");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    n = snprintf(sql + used, sizeof(sql) - used,
        ") ON CONFLICT(name) DO UPDATE SET quantity=r2_fridge_items.quantity+excluded.quantity,");
    if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
    used += (size_t)n;
    if (update_metadata) {
        n = snprintf(sql + used, sizeof(sql) - used,
            "description=excluded.description,fullness=excluded.fullness,energy=excluded.energy,"
            "ingredients=excluded.ingredients,taste=excluded.taste,");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    if (has_qty) {
        n = snprintf(sql + used, sizeof(sql) - used, "qty=r2_fridge_items.quantity+excluded.quantity,");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    if (has_amount) {
        n = snprintf(sql + used, sizeof(sql) - used, "amount=r2_fridge_items.quantity+excluded.quantity,");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    n = snprintf(sql + used, sizeof(sql) - used, "updated_at=CURRENT_TIMESTAMP");
    if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(fridge_db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int i = 1;
        bind_text(st, i++, name);
        bind_text(st, i++, description ? description : "");
        sqlite3_bind_int(st, i++, quantity);
        sqlite3_bind_double(st, i++, fullness);
        sqlite3_bind_double(st, i++, energy);
        bind_text(st, i++, ingredients ? ingredients : "");
        bind_text(st, i++, taste ? taste : "");
        if (has_qty) sqlite3_bind_int(st, i++, quantity);
        if (has_amount) sqlite3_bind_int(st, i++, quantity);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    return rc;
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
    if (qty <= 1) {
        if (sqlite3_prepare_v2(fridge_db, "DELETE FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &st, NULL) != SQLITE_OK) return -1;
    } else {
        char sql[512];
        size_t used = 0;
        int n = snprintf(sql, sizeof(sql), "UPDATE r2_fridge_items SET quantity=quantity-1");
        if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
        used = (size_t)n;
        if (fridge_column_exists("qty")) {
            n = snprintf(sql + used, sizeof(sql) - used, ",qty=quantity-1");
            if (n < 0 || (size_t)n >= sizeof(sql) - used) return -1;
            used += (size_t)n;
        }
        if (fridge_column_exists("amount")) {
            n = snprintf(sql + used, sizeof(sql) - used, ",amount=quantity-1");
            if (n < 0 || (size_t)n >= sizeof(sql) - used) return -1;
            used += (size_t)n;
        }
        n = snprintf(sql + used, sizeof(sql) - used,
                     ",updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return -1;
        if (sqlite3_prepare_v2(fridge_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    }
    bind_text(st, 1, food);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}
static int fridge_restore_one(const char *food, const char *description, double fullness, double energy,
                             const char *ingredients, const char *taste)
{
    return fridge_insert_item_compat(food, description, 1, fullness, energy,
                                     ingredients, taste, 0) == SQLITE_DONE ? 0 : -1;
}
int r2_fridge_add_item(const char *name, const char *description, int quantity,
                       double fullness, double energy, const char *ingredients, const char *taste)
{
    if (!name || !*name || strlen(name) > REALITY_MAX_TEXT || !fridge_db ||
        quantity < 1 || !isfinite(fullness) || !isfinite(energy) ||
        fullness < 0.0 || fullness > 100.0 ||
        energy < 0.0 || energy > 100.0) return -1;
    pthread_mutex_lock(&fridge_lock);
    int seed_rc = fridge_seed_if_empty_locked();
    sqlite3_stmt *st = NULL;
    int rc = seed_rc == 0 ? sqlite3_prepare_v2(fridge_db,
        "SELECT quantity FROM r2_fridge_items WHERE name=? COLLATE NOCASE",
        -1, &st, NULL) : SQLITE_ERROR;
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            sqlite3_int64 existing_quantity = sqlite3_column_int64(st, 0);
            rc = existing_quantity > 0 &&
                 existing_quantity <= (sqlite3_int64)INT_MAX - quantity
                ? SQLITE_OK : SQLITE_TOOBIG;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_OK;
        }
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (rc == SQLITE_OK) {
        rc = fridge_insert_item_compat(name, description ? description : "", quantity,
                                       fullness, energy, ingredients, taste, 1);
    }
    if (rc == SQLITE_DONE) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(fridge_db, "SELECT quantity FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &q, NULL) == SQLITE_OK) {
            bind_text(q, 1, name);
            if (sqlite3_step(q) == SQLITE_ROW)
                fridge_mirror_write_warn(name, description, sqlite3_column_int(q, 0),
                                         fullness, energy, "stock addition");
        }
        if (q) sqlite3_finalize(q);
    }
    pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_DONE ? 0 : -1;
}

typedef struct {
    char operation[32];
    char name[REALITY_MAX_TEXT + 1];
    int reality_exists;
    char reality_description[REALITY_MAX_TEXT + 1];
    int reality_quantity;
    char reality_container[REALITY_MAX_TEXT + 1];
    char reality_owner[REALITY_MAX_TEXT + 1];
    sqlite3_int64 item_memory_max_id;
    int fridge_exists;
    char fridge_description[REALITY_MAX_TEXT + 1];
    int fridge_quantity;
    double fridge_fullness;
    double fridge_energy;
    char fridge_ingredients[1024];
    char fridge_taste[1024];
    char phase[24];
} InventoryTransferSnapshot;

/* Caller holds reality_lock. The snapshot is persisted before either database
   is changed, so startup can restore both sides after an interrupted transfer. */
static int inventory_transfer_snapshot_locked(const char *name,
                                               InventoryTransferSnapshot *snapshot)
{
    if (!reality_db || !fridge_db || !name || !*name || !snapshot) return -1;
    memset(snapshot, 0, sizeof(*snapshot));
    snprintf(snapshot->name, sizeof(snapshot->name), "%s", name);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT description,quantity,container,owner FROM r2_reality_objects "
        "WHERE name=? COLLATE NOCASE", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const unsigned char *description = sqlite3_column_text(st, 0);
            const unsigned char *container = sqlite3_column_text(st, 2);
            const unsigned char *owner = sqlite3_column_text(st, 3);
            snapshot->reality_exists = 1;
            snprintf(snapshot->reality_description, sizeof(snapshot->reality_description),
                     "%s", description ? (const char *)description : "");
            snapshot->reality_quantity = sqlite3_column_int(st, 1);
            snprintf(snapshot->reality_container, sizeof(snapshot->reality_container),
                     "%s", container ? (const char *)container : "");
            snprintf(snapshot->reality_owner, sizeof(snapshot->reality_owner),
                     "%s", owner ? (const char *)owner : "R2");
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_OK;
        }
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK) return -1;

    st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "SELECT COALESCE(MAX(id),0) FROM r2_reality_item_memory", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            snapshot->item_memory_max_id = sqlite3_column_int64(st, 0);
            rc = SQLITE_OK;
        }
    }
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK) return -1;

    pthread_mutex_lock(&fridge_lock);
    st = NULL;
    rc = sqlite3_prepare_v2(fridge_db,
        "SELECT description,quantity,fullness,energy,ingredients,taste "
        "FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const unsigned char *description = sqlite3_column_text(st, 0);
            const unsigned char *ingredients = sqlite3_column_text(st, 4);
            const unsigned char *taste = sqlite3_column_text(st, 5);
            snapshot->fridge_exists = 1;
            snprintf(snapshot->fridge_description, sizeof(snapshot->fridge_description),
                     "%s", description ? (const char *)description : "");
            snapshot->fridge_quantity = sqlite3_column_int(st, 1);
            snapshot->fridge_fullness = sqlite3_column_double(st, 2);
            snapshot->fridge_energy = sqlite3_column_double(st, 3);
            snprintf(snapshot->fridge_ingredients, sizeof(snapshot->fridge_ingredients),
                     "%s", ingredients ? (const char *)ingredients : "");
            snprintf(snapshot->fridge_taste, sizeof(snapshot->fridge_taste),
                     "%s", taste ? (const char *)taste : "");
            rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_OK;
        }
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Caller holds reality_lock. */
static int inventory_transfer_journal_begin_locked(const char *operation,
                                                    const InventoryTransferSnapshot *snapshot)
{
    if (!reality_db || !operation || !snapshot) return -1;
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_inventory_transfer_journal("
        "id,operation,item_name,reality_exists,reality_description,reality_quantity,"
        "reality_container,reality_owner,item_memory_max_id,fridge_exists,fridge_description,"
        "fridge_quantity,fridge_fullness,fridge_energy,fridge_ingredients,fridge_taste,phase) "
        "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'pending')", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int i = 1;
        sqlite3_bind_text(st, i++, operation, -1, SQLITE_TRANSIENT);
        bind_text(st, i++, snapshot->name);
        sqlite3_bind_int(st, i++, snapshot->reality_exists);
        bind_text(st, i++, snapshot->reality_description);
        sqlite3_bind_int(st, i++, snapshot->reality_quantity);
        bind_text(st, i++, snapshot->reality_container);
        bind_text(st, i++, snapshot->reality_owner);
        sqlite3_bind_int64(st, i++, snapshot->item_memory_max_id);
        sqlite3_bind_int(st, i++, snapshot->fridge_exists);
        bind_text(st, i++, snapshot->fridge_description);
        sqlite3_bind_int(st, i++, snapshot->fridge_quantity);
        sqlite3_bind_double(st, i++, snapshot->fridge_fullness);
        sqlite3_bind_double(st, i++, snapshot->fridge_energy);
        bind_text(st, i++, snapshot->fridge_ingredients);
        bind_text(st, i++, snapshot->fridge_taste);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int inventory_transfer_journal_mark_committed(void)
{
    pthread_mutex_lock(&reality_lock);
    if (!reality_db) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_inventory_transfer_journal SET phase='committed' "
            "WHERE id=1 AND phase='pending'", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 1)
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    else rc = SQLITE_ERROR;
    if (rc != SQLITE_OK) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&reality_lock);
    return rc == SQLITE_OK ? 0 : -1;
}

static int inventory_transfer_journal_clear_locked(void)
{
    if (!reality_db) return -1;
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(reality_db, "DELETE FROM r2_inventory_transfer_journal WHERE id=1",
                          NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    return rc == SQLITE_OK ? 0 : -1;
}

static int inventory_transfer_journal_clear(void)
{
    pthread_mutex_lock(&reality_lock);
    int rc = inventory_transfer_journal_clear_locked();
    pthread_mutex_unlock(&reality_lock);
    return rc;
}

/* Caller holds reality_lock. Reapplying a restore is safe if a second crash
   happens while recovery is running: the journal is cleared only after both
   authoritative databases have been restored. */
static int inventory_transfer_restore_fridge_locked(const InventoryTransferSnapshot *snapshot)
{
    if (!fridge_db || !snapshot) return -1;
    pthread_mutex_lock(&fridge_lock);
    int rc = sqlite3_exec(fridge_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(fridge_db,
            "DELETE FROM r2_fridge_items WHERE name=? COLLATE NOCASE", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, snapshot->name);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE && snapshot->fridge_exists) {
        rc = fridge_insert_item_compat(snapshot->name, snapshot->fridge_description,
            snapshot->fridge_quantity, snapshot->fridge_fullness, snapshot->fridge_energy,
            snapshot->fridge_ingredients, snapshot->fridge_taste, 1);
    }
    if (rc == SQLITE_DONE) rc = sqlite3_exec(fridge_db, "COMMIT", NULL, NULL, NULL);
    else (void)sqlite3_exec(fridge_db, "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) (void)sqlite3_exec(fridge_db, "ROLLBACK", NULL, NULL, NULL);
    if (rc == SQLITE_OK) {
        if (snapshot->fridge_exists)
            fridge_mirror_write_warn(snapshot->name, snapshot->fridge_description,
                snapshot->fridge_quantity, snapshot->fridge_fullness,
                snapshot->fridge_energy, "interrupted-transfer recovery");
        else
            fridge_mirror_remove(snapshot->name);
    }
    pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Caller holds reality_lock. */
static int inventory_transfer_restore_reality_locked(const InventoryTransferSnapshot *snapshot)
{
    if (!reality_db || !snapshot) return -1;
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (rc == SQLITE_OK && snapshot->reality_exists) {
        rc = sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_objects(name,description,quantity,container,owner) "
            "VALUES(?,?,?,?,?) ON CONFLICT(name) DO UPDATE SET description=excluded.description,"
            "quantity=excluded.quantity,container=excluded.container,owner=excluded.owner,"
            "updated_at=CURRENT_TIMESTAMP", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            bind_text(st, 1, snapshot->name);
            bind_text(st, 2, snapshot->reality_description);
            sqlite3_bind_int(st, 3, snapshot->reality_quantity);
            bind_text(st, 4, snapshot->reality_container);
            bind_text(st, 5, snapshot->reality_owner);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    } else if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(reality_db,
            "DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            bind_text(st, 1, snapshot->name);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }
    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(reality_db,
            "DELETE FROM r2_reality_item_memory WHERE id>? AND item_name=? COLLATE NOCASE",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, snapshot->item_memory_max_id);
            bind_text(st, 2, snapshot->name);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
    }
    if (rc == SQLITE_DONE) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    else (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    if (rc == SQLITE_OK) {
        if (snapshot->reality_exists) {
            if (mirror_write(snapshot->name, snapshot->reality_description,
                             snapshot->reality_quantity, snapshot->reality_container) != 0)
                fprintf(stderr, "[R2 Reality] Could not restore room mirror for '%s' after transfer recovery.\n",
                        snapshot->name);
        } else {
            mirror_remove(snapshot->name, "pockets");
        }
    }
    return rc == SQLITE_OK ? 0 : -1;
}

static int inventory_transfer_recover_locked(void)
{
    if (!reality_db) return -1;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT operation,item_name,reality_exists,reality_description,reality_quantity,"
        "reality_container,reality_owner,item_memory_max_id,fridge_exists,fridge_description,"
        "fridge_quantity,fridge_fullness,fridge_energy,fridge_ingredients,fridge_taste,phase "
        "FROM r2_inventory_transfer_journal WHERE id=1", -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        return 0;
    }
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    InventoryTransferSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    const unsigned char *value = sqlite3_column_text(st, 0);
    snprintf(snapshot.operation, sizeof(snapshot.operation), "%s", value ? (const char *)value : "");
    value = sqlite3_column_text(st, 1);
    snprintf(snapshot.name, sizeof(snapshot.name), "%s", value ? (const char *)value : "");
    snapshot.reality_exists = sqlite3_column_int(st, 2);
    value = sqlite3_column_text(st, 3);
    snprintf(snapshot.reality_description, sizeof(snapshot.reality_description), "%s", value ? (const char *)value : "");
    snapshot.reality_quantity = sqlite3_column_int(st, 4);
    value = sqlite3_column_text(st, 5);
    snprintf(snapshot.reality_container, sizeof(snapshot.reality_container), "%s", value ? (const char *)value : "");
    value = sqlite3_column_text(st, 6);
    snprintf(snapshot.reality_owner, sizeof(snapshot.reality_owner), "%s", value ? (const char *)value : "R2");
    snapshot.item_memory_max_id = sqlite3_column_int64(st, 7);
    snapshot.fridge_exists = sqlite3_column_int(st, 8);
    value = sqlite3_column_text(st, 9);
    snprintf(snapshot.fridge_description, sizeof(snapshot.fridge_description), "%s", value ? (const char *)value : "");
    snapshot.fridge_quantity = sqlite3_column_int(st, 10);
    snapshot.fridge_fullness = sqlite3_column_double(st, 11);
    snapshot.fridge_energy = sqlite3_column_double(st, 12);
    value = sqlite3_column_text(st, 13);
    snprintf(snapshot.fridge_ingredients, sizeof(snapshot.fridge_ingredients), "%s", value ? (const char *)value : "");
    value = sqlite3_column_text(st, 14);
    snprintf(snapshot.fridge_taste, sizeof(snapshot.fridge_taste), "%s", value ? (const char *)value : "");
    value = sqlite3_column_text(st, 15);
    snprintf(snapshot.phase, sizeof(snapshot.phase), "%s", value ? (const char *)value : "");
    sqlite3_finalize(st);

    if (!strcmp(snapshot.phase, "committed"))
        return inventory_transfer_journal_clear_locked();
    if (strcmp(snapshot.phase, "pending") != 0) return -1;
    if (inventory_transfer_restore_fridge_locked(&snapshot) != 0 ||
        inventory_transfer_restore_reality_locked(&snapshot) != 0)
        return -1;
    if (inventory_transfer_journal_clear_locked() != 0) return -1;
    fprintf(stderr, "[R2 Reality] Rolled back interrupted %s transfer for '%s' to its pre-transfer state.\n",
            snapshot.operation, snapshot.name);
    return 0;
}

static int inventory_transfer_recover(void)
{
    pthread_mutex_lock(&reality_lock);
    int rc = inventory_transfer_recover_locked();
    pthread_mutex_unlock(&reality_lock);
    return rc;
}


int r2_fridge_take(const char *food)
{
    if (!food || !*food || strlen(food) > REALITY_MAX_TEXT ||
        !fridge_db || !r2_reality_is_initialized() || !reality_is_home())
        return -1;

    pthread_mutex_lock(&purchase_lock);
    if (!r2_reality_is_initialized() || !reality_is_home() || !fridge_db) {
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }

    InventoryTransferSnapshot snapshot;
    pthread_mutex_lock(&reality_lock);
    int rc = inventory_transfer_recover_locked();
    if (rc == 0) rc = inventory_transfer_snapshot_locked(food, &snapshot);
    if (rc == 0 && (!snapshot.fridge_exists || snapshot.fridge_quantity < 1))
        rc = -1;
    if (rc == 0)
        rc = inventory_transfer_journal_begin_locked("take_from_fridge", &snapshot);
    pthread_mutex_unlock(&reality_lock);
    if (rc != 0) {
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }

    char desc[REALITY_MAX_TEXT + 1] = {0};
    char ingredients[1024] = {0}, taste[1024] = {0};
    int qty = 0;
    double fullness = 0.0, energy = 0.0;
    pthread_mutex_lock(&fridge_lock);
    rc = fridge_seed_if_empty_locked();
    if (rc == 0)
        rc = fridge_change_one(food, desc, sizeof(desc), &qty, &fullness, &energy,
                               ingredients, sizeof(ingredients), taste, sizeof(taste));
    if (rc == 0) {
        if (qty == 1) fridge_mirror_remove(food);
        else fridge_mirror_write_warn(food, desc, qty - 1, fullness, energy, "stock take");
    }
    pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Fridge] CRITICAL: could not recover failed take of '%s'; journal retained.\n", food);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }

#ifdef R2_INVENTORY_TEST_FAILURE_INJECTION
    if (getenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_TAKE")) {
        /* Leave the pending journal exactly as a process crash would. */
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
#endif

    if (r2_reality_add_item(food, desc, "pockets", 1) != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Fridge] CRITICAL: failed-take rollback for '%s' is pending recovery.\n", food);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
    if (inventory_transfer_journal_mark_committed() != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Fridge] CRITICAL: transfer commit recovery for '%s' is pending.\n", food);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
    if (inventory_transfer_journal_clear() != 0)
        fprintf(stderr, "[R2 Fridge] Committed take journal retained for startup cleanup ('%s').\n", food);

    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 took %s from the fridge into his pockets.", food);
    snprintf(details, sizeof(details), "Food=%s; source=fridge database; destination=pockets; one verified stock unit consumed.", food);
    bridge_event("fridge_item_taken", summary, details, 1, 0);
    pthread_mutex_unlock(&purchase_lock);
    return 0;
}
int r2_reality_fridge_eat(const char *food, double fullness)
{
    /* Keep the legacy parameter for ABI compatibility; stock metrics are
       authoritative and must not be overridden by an action's guess. */
    (void)fullness;
    if (!food || !*food || !fridge_db || !r2_reality_is_initialized() || !reality_is_home()) return -1;
    char desc[REALITY_MAX_TEXT + 1] = {0}, ingredients[1024] = {0}, taste[1024] = {0};
    int qty = 0; double stored_fullness = 0.0, energy = 0.0;
    pthread_mutex_lock(&fridge_lock);
    int rc = fridge_seed_if_empty_locked();
    if (rc == 0) rc = fridge_change_one(food, desc, sizeof(desc), &qty, &stored_fullness, &energy,
                                         ingredients, sizeof(ingredients), taste, sizeof(taste));
    /* Fridge stock's authoritative metric wins over a caller-supplied value:
       non-nutritive stock must never become a meal via a positive override. */
    if (rc == 0 && (!isfinite(stored_fullness) || stored_fullness <= 0.0)) {
        int restore_rc = fridge_restore_one(food, desc, stored_fullness, energy, ingredients, taste);
        if (restore_rc != 0) {
            if (qty == 1) fridge_mirror_remove(food);
            else fridge_mirror_write_warn(food, desc, qty - 1, stored_fullness, energy,
                                           "stock restoration");
            fprintf(stderr, "[R2 Fridge] Failed to restore non-nutritive stock '%s'.\n", food);
        }
        rc = -1;
    }
    if (rc == 0) {
        if (qty == 1) fridge_mirror_remove(food);
        else fridge_mirror_write_warn(food, desc, qty - 1, stored_fullness, energy, "meal stock consumption");
    }
    pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    fullness = stored_fullness;
    rc = reality_eat_internal(food, fullness, 0, energy);
    if (rc != 0) {
        pthread_mutex_lock(&fridge_lock);
        int restore_rc = fridge_restore_one(food, desc, stored_fullness, energy, ingredients, taste);
        if (restore_rc == 0)
            fridge_mirror_write_warn(food, desc, qty, stored_fullness, energy, "failed-meal rollback");
        else {
            if (qty == 1) fridge_mirror_remove(food);
            else fridge_mirror_write_warn(food, desc, qty - 1, stored_fullness, energy,
                                           "stock restoration");
        }
        pthread_mutex_unlock(&fridge_lock);
        if (restore_rc != 0)
            fprintf(stderr, "[R2 Fridge] Failed to restore '%s' after an unsuccessful meal; stock may need inspection.\n", food);
        return -1;
    }
    pthread_mutex_lock(&fridge_lock); rc = fridge_seed_if_empty_locked(); pthread_mutex_unlock(&fridge_lock);
    if (rc != 0) return -1;
    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 ate %s directly from the fridge.", food);
    snprintf(details, sizeof(details), "Food=%s; source=fridge database; fullness=%.1f; an empty fridge remains empty.", food, stored_fullness);
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

static const char *legacy_column_expr(const char *table, const char *column,
                                      const char *fallback)
{
    return legacy_column_exists(table, column) ? column : fallback;
}

static int legacy_table_has_rows(const char *table)
{
    char sql[256];
    int n = snprintf(sql, sizeof(sql), "SELECT 1 FROM legacy.%s LIMIT 1", table);
    if (n <= 0 || (size_t)n >= sizeof(sql)) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(reality_db, sql, -1, &st, NULL) == SQLITE_OK)
        found = sqlite3_step(st) == SQLITE_ROW;
    if (st) sqlite3_finalize(st);
    return found;
}

/* Copy a legacy table using explicit destination columns. Missing optional
 * fields receive a neutral/legacy-compatible expression; required identity
 * fields must exist or migration fails rather than silently dropping data.
 * History tables omit auto IDs so old rows can coexist with new DB rows. */
static int legacy_copy_table(const char *table,
                             const char *const *columns,
                             const char *const *fallbacks, size_t column_count,
                             const char *const *required, size_t required_count,
                             int replace_conflicts)
{
    if (!legacy_table_exists(table)) return 0;
    for (size_t i = 0; i < required_count; ++i) {
        if (!legacy_column_exists(table, required[i])) {
            fprintf(stderr,
                    "[R2 Reality] Legacy table %s is missing required column %s; refusing incomplete migration.\n",
                    table, required[i]);
            return -1;
        }
    }

    char sql[8192];
    size_t used = 0;
#define LEGACY_APPEND_SQL(...) do { \
        int written = snprintf(sql + used, sizeof(sql) - used, __VA_ARGS__); \
        if (written < 0 || (size_t)written >= sizeof(sql) - used) { \
            fprintf(stderr, "[R2 Reality] Legacy migration SQL exceeded its bound for %s.\n", table); \
            return -1; \
        } \
        used += (size_t)written; \
    } while (0)
    LEGACY_APPEND_SQL("INSERT OR %s INTO main.%s (",
                      replace_conflicts ? "REPLACE" : "IGNORE", table);
    for (size_t i = 0; i < column_count; ++i)
        LEGACY_APPEND_SQL("%s%s", i ? "," : "", columns[i]);
    LEGACY_APPEND_SQL(") SELECT ");
    for (size_t i = 0; i < column_count; ++i) {
        const char *expr = legacy_column_expr(table, columns[i], fallbacks[i]);
        if (!expr) {
            fprintf(stderr,
                    "[R2 Reality] No safe fallback for legacy column %s.%s.\n",
                    table, columns[i]);
            return -1;
        }
        LEGACY_APPEND_SQL("%s%s", i ? "," : "", expr);
    }
    LEGACY_APPEND_SQL(" FROM legacy.%s", table);
#undef LEGACY_APPEND_SQL
    return exec_sql(sql);
}

/* Merge learned preference aggregates instead of overwriting ratings already
 * recorded in the destination database. The migration transaction makes this
 * safe to retry after any interrupted startup. */
static int legacy_merge_preference_table(const char *table, const char *key_column)
{
    if (!legacy_table_exists(table)) return 0;
    if (!legacy_column_exists(table, key_column)) {
        fprintf(stderr, "[R2 Reality] Legacy preference table %s lacks %s.\n",
                table, key_column);
        return -1;
    }
    const char *sum = legacy_column_expr(table, "satisfaction_sum", "0");
    const char *count = legacy_column_expr(table, "rating_count", "0");
    const char *updated = legacy_column_expr(table, "updated_at", "0");
    char sql[4096];
    int n = snprintf(sql, sizeof(sql),
        "INSERT INTO main.%s(%s,satisfaction_sum,rating_count,updated_at) "
        "SELECT %s,COALESCE(%s,0),COALESCE(%s,0),COALESCE(%s,0) "
        "FROM legacy.%s WHERE 1 "
        "ON CONFLICT(%s) DO UPDATE SET "
        "satisfaction_sum=satisfaction_sum+excluded.satisfaction_sum,"
        "rating_count=rating_count+excluded.rating_count,"
        "updated_at=MAX(updated_at,excluded.updated_at)",
        table, key_column, key_column, sum, count, updated,
        table, key_column);
    if (n <= 0 || (size_t)n >= sizeof(sql)) return -1;
    return exec_sql(sql);
}

/* One-time, transactional migration from older releases where Reality-owned
 * tables lived inside r2_memory.db. The source database is read only here and
 * is never cleared or rewritten; Life Log and searchable memory stay there. */
static int migrate_legacy_reality(void)
{
    if (access(R2_DIARY_DATABASE, F_OK) != 0) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(reality_db,
        "SELECT value FROM r2_reality_meta WHERE key='legacy_reality_migrated'",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
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
    if (rc != SQLITE_DONE) {
        (void)sqlite3_exec(reality_db, "DETACH DATABASE legacy", NULL, NULL, NULL);
        return -1;
    }

    int result = exec_sql("BEGIN IMMEDIATE");
    const char *container_cols[] = {"name","kind","description","parent"};
    const char *container_defaults[] = {NULL,"'container'","NULL","NULL"};
    const char *container_required[] = {"name"};
    const char *fact_cols[] = {"key","value","evidence","updated_at"};
    const char *fact_defaults[] = {NULL,NULL,"NULL","CURRENT_TIMESTAMP"};
    const char *fact_required[] = {"key","value"};
    const char *tick_cols[] = {"previous_tick","current_tick","elapsed_seconds","created_at"};
    const char *tick_defaults[] = {NULL,NULL,"current_tick-previous_tick","CURRENT_TIMESTAMP"};
    const char *tick_required[] = {"previous_tick","current_tick"};
    const char *object_cols[] = {"name","description","quantity","container","owner","created_at","updated_at"};
    const char *object_container_default = legacy_column_exists("r2_reality_objects", "location")
        ? "location" : "'room'";
    const char *object_defaults[] = {NULL,"NULL","1",object_container_default,"'R2'","CURRENT_TIMESTAMP","CURRENT_TIMESTAMP"};
    const char *object_required[] = {"name"};
    const char *legacy_collected_at_default =
        legacy_column_exists("r2_reality_item_memory", "created_at")
            ? "CAST(strftime('%s',created_at) AS INTEGER)"
            : "CAST(strftime('%s','now') AS INTEGER)";
    const char *legacy_decay_at_default =
        legacy_column_exists("r2_reality_item_memory", "collected_at")
            ? "collected_at" : legacy_collected_at_default;
    const char *item_memory_cols[] = {"item_name","description","exact_quantity","approximate_quantity","precision","collected_at","last_decay_at"};
    const char *item_memory_defaults[] = {NULL,"NULL","NULL","NULL","'exact'",legacy_collected_at_default,legacy_decay_at_default};
    const char *item_memory_required[] = {"item_name"};
    const char *food_experience_cols[] = {"food_name","ingredients","fullness","energy_bonus","satisfaction","notes","eaten_at","rated_at"};
    const char *food_experience_defaults[] = {NULL,"NULL",NULL,"0.0","NULL","NULL",NULL,"NULL"};
    const char *food_experience_required[] = {"food_name","fullness","eaten_at"};
    const char *food_meta_cols[] = {"key","value"};
    const char *food_meta_defaults[] = {NULL,NULL};
    const char *food_meta_required[] = {"key","value"};
    const char *money_cols[] = {"id","cash_cents","bank_cents","updated_at"};
    const char *money_defaults[] = {"1",NULL,NULL,"CURRENT_TIMESTAMP"};
    const char *money_required[] = {"cash_cents","bank_cents"};
    const char *tv_state_cols[] = {"id","power","source_kind","source_value","updated_at"};
    const char *tv_state_defaults[] = {"1",NULL,NULL,NULL,"CURRENT_TIMESTAMP"};
    const char *tv_state_required[] = {"power","source_kind","source_value"};
    const char *tv_device_cols[] = {"name","connection_kind","port","connected","updated_at"};
    const char *tv_device_defaults[] = {NULL,NULL,NULL,"1","CURRENT_TIMESTAMP"};
    const char *tv_device_required[] = {"name","connection_kind","port"};
    const char *tape_cols[] = {"media_path","position_seconds","updated_at"};
    const char *tape_defaults[] = {NULL,"0","CURRENT_TIMESTAMP"};
    const char *tape_required[] = {"media_path"};
    const char *vcr_state_cols[] = {"id","cassette_path","transport","updated_at"};
    const char *vcr_state_defaults[] = {"1","NULL",NULL,"CURRENT_TIMESTAMP"};
    const char *vcr_state_required[] = {"transport"};

    const struct { const char *table, *sql; } simple_copies[] = {
        {"r2_reality_meta", "INSERT OR IGNORE INTO main.r2_reality_meta(key,value) SELECT key,value FROM legacy.r2_reality_meta"}
    };
    for (size_t i = 0; result == 0 && i < sizeof(simple_copies)/sizeof(simple_copies[0]); ++i) {
        if (legacy_table_exists(simple_copies[i].table))
            result = exec_sql(simple_copies[i].sql);
    }
    if (result == 0)
        result = legacy_copy_table("r2_reality_containers", container_cols, container_defaults,
                                   sizeof(container_cols)/sizeof(container_cols[0]),
                                   container_required, sizeof(container_required)/sizeof(container_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_reality_self_facts", fact_cols, fact_defaults,
                                   sizeof(fact_cols)/sizeof(fact_cols[0]),
                                   fact_required, sizeof(fact_required)/sizeof(fact_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_reality_ticks", tick_cols, tick_defaults,
                                   sizeof(tick_cols)/sizeof(tick_cols[0]),
                                   tick_required, sizeof(tick_required)/sizeof(tick_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_reality_item_memory", item_memory_cols, item_memory_defaults,
                                   sizeof(item_memory_cols)/sizeof(item_memory_cols[0]),
                                   item_memory_required, sizeof(item_memory_required)/sizeof(item_memory_required[0]), 0);

    if (result == 0 && legacy_table_exists("r2_reality_objects") &&
        legacy_column_exists("r2_reality_objects", "container")) {
        result = exec_sql(
            "INSERT OR IGNORE INTO main.r2_reality_containers(name,kind,description,parent) "
            "SELECT DISTINCT legacy_objects.container,'container','Recovered legacy object container','room' "
            "FROM legacy.r2_reality_objects AS legacy_objects "
            "LEFT JOIN main.r2_reality_containers AS known ON known.name=legacy_objects.container "
            "WHERE legacy_objects.container IS NOT NULL AND legacy_objects.container<>'' AND known.name IS NULL");
    }
    if (result == 0)
        result = legacy_copy_table("r2_reality_objects", object_cols, object_defaults,
                                   sizeof(object_cols)/sizeof(object_cols[0]),
                                   object_required, sizeof(object_required)/sizeof(object_required[0]), 0);

    if (result == 0 && legacy_table_exists("r2_reality_self")) {
        if (!legacy_column_exists("r2_reality_self", "id") ||
            !legacy_column_exists("r2_reality_self", "hunger")) {
            fprintf(stderr, "[R2 Reality] Legacy self-state table lacks id/hunger; refusing incomplete migration.\n");
            result = -1;
        } else {
            const char *satisfaction = legacy_column_expr("r2_reality_self", "satisfaction", "100");
            const char *since = legacy_column_expr("r2_reality_self", "seconds_since_meal", "0");
            const char *sleep = legacy_column_expr("r2_reality_self", "sleepiness", "0");
            const char *energy = legacy_column_expr("r2_reality_self", "energy", "100");
            const char *last = legacy_column_expr("r2_reality_self", "last_tick", "0");
            const char *updated = legacy_column_expr("r2_reality_self", "updated_at", "CURRENT_TIMESTAMP");
            char sql[2048];
            int n = snprintf(sql, sizeof(sql),
                "INSERT OR REPLACE INTO main.r2_reality_self"
                "(id,hunger,satisfaction,seconds_since_meal,sleepiness,energy,last_tick,updated_at) "
                "SELECT id,hunger,%s,%s,%s,%s,%s,%s FROM legacy.r2_reality_self",
                satisfaction, since, sleep, energy, last, updated);
            result = n > 0 && (size_t)n < sizeof(sql) ? exec_sql(sql) : -1;
        }
    }

    if (result == 0)
        result = legacy_copy_table("r2_food_experiences", food_experience_cols, food_experience_defaults,
                                   sizeof(food_experience_cols)/sizeof(food_experience_cols[0]),
                                   food_experience_required, sizeof(food_experience_required)/sizeof(food_experience_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_food_experience_meta", food_meta_cols, food_meta_defaults,
                                   sizeof(food_meta_cols)/sizeof(food_meta_cols[0]),
                                   food_meta_required, sizeof(food_meta_required)/sizeof(food_meta_required[0]), 0);
    if (result == 0)
        result = legacy_merge_preference_table("r2_food_preferences", "food_name");
    if (result == 0)
        result = legacy_merge_preference_table("r2_food_ingredient_preferences", "ingredient");

    if (result == 0)
        result = legacy_copy_table("r2_money_account", money_cols, money_defaults,
                                   sizeof(money_cols)/sizeof(money_cols[0]),
                                   money_required, sizeof(money_required)/sizeof(money_required[0]), 1);
    if (result == 0)
        result = legacy_copy_table("r2_tv_devices", tv_device_cols, tv_device_defaults,
                                   sizeof(tv_device_cols)/sizeof(tv_device_cols[0]),
                                   tv_device_required, sizeof(tv_device_required)/sizeof(tv_device_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_tv_vcr_tapes", tape_cols, tape_defaults,
                                   sizeof(tape_cols)/sizeof(tape_cols[0]),
                                   tape_required, sizeof(tape_required)/sizeof(tape_required[0]), 0);
    if (result == 0)
        result = legacy_copy_table("r2_tv_state", tv_state_cols, tv_state_defaults,
                                   sizeof(tv_state_cols)/sizeof(tv_state_cols[0]),
                                   tv_state_required, sizeof(tv_state_required)/sizeof(tv_state_required[0]), 1);
    if (result == 0)
        result = legacy_copy_table("r2_tv_vcr_state", vcr_state_cols, vcr_state_defaults,
                                   sizeof(vcr_state_cols)/sizeof(vcr_state_cols[0]),
                                   vcr_state_required, sizeof(vcr_state_required)/sizeof(vcr_state_required[0]), 1);

    if (result == 0 && legacy_table_exists("r2_reality_meta"))
        result = exec_sql(
            "UPDATE main.r2_reality_meta SET value=(SELECT value FROM legacy.r2_reality_meta "
            "WHERE legacy.r2_reality_meta.key=main.r2_reality_meta.key) "
            "WHERE key IN (SELECT key FROM legacy.r2_reality_meta)");
    /* If a legacy account existed, its balance must be materialized into the
       physical wallet on first migration rather than replaced by the new-user
       seed. The physical files still win when they already exist. */
    if (result == 0 && legacy_table_has_rows("r2_money_account"))
        result = exec_sql(
            "INSERT INTO main.r2_reality_meta(key,value) VALUES('initial_cash_seeded','1') "
            "ON CONFLICT(key) DO UPDATE SET value='1'");
    if (result == 0)
        result = exec_sql(
            "INSERT INTO main.r2_reality_meta(key,value) VALUES('legacy_reality_migrated','yes') "
            "ON CONFLICT(key) DO UPDATE SET value='yes'");

    if (result == 0) {
        if (exec_sql("COMMIT") != 0) result = -1;
    }
    if (result != 0) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    (void)sqlite3_exec(reality_db, "DETACH DATABASE legacy", NULL, NULL, NULL);
    if (result == 0)
        fprintf(stderr,
                "[R2 Reality] Legacy Reality tables migrated into r2_reality.db; r2_memory.db remains intact.\n");
    return result;
}

static int reality_column_exists(const char *table, const char *column)
{
    char sql[256];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(reality_db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && !strcasecmp((const char *)name, column)) {
                found = 1;
                break;
            }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

static int reality_table_exists(const char *table)
{
    sqlite3_stmt *st = NULL;
    int exists = 0;
    if (sqlite3_prepare_v2(reality_db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(st) == SQLITE_ROW;
    }
    if (st) sqlite3_finalize(st);
    return exists;
}

static int reality_ensure_existing_column(const char *table, const char *column,
                                          const char *definition)
{
    /* Only alter tables that already exist. Fresh tables are created by the
       canonical schema below; this pass must run before its indexes/inserts. */
    if (!reality_table_exists(table) || reality_column_exists(table, column))
        return 0;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "ALTER TABLE %s ADD COLUMN %s %s;",
                     table, column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
    if (exec_sql(sql) != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate %s.%s: %s\n",
                table, column, sqlite3_errmsg(reality_db));
        return -1;
    }
    return 0;
}

static int migrate_existing_reality_schema(void)
{
    static const char *const columns[][3] = {
        {"r2_reality_item_memory","description","TEXT"},
        {"r2_reality_item_memory","exact_quantity","INTEGER"},
        {"r2_reality_item_memory","approximate_quantity","INTEGER"},
        {"r2_reality_item_memory","precision","TEXT NOT NULL DEFAULT 'exact'"},
        {"r2_reality_item_memory","collected_at","INTEGER NOT NULL DEFAULT 0"},
        {"r2_reality_item_memory","last_decay_at","INTEGER NOT NULL DEFAULT 0"},

        {"r2_food_experiences","ingredients","TEXT"},
        {"r2_food_experiences","fullness","REAL NOT NULL DEFAULT 0"},
        {"r2_food_experiences","energy_bonus","REAL NOT NULL DEFAULT 0"},
        {"r2_food_experiences","satisfaction","INTEGER"},
        {"r2_food_experiences","notes","TEXT"},
        {"r2_food_experiences","eaten_at","INTEGER NOT NULL DEFAULT 0"},
        {"r2_food_experiences","rated_at","INTEGER"},

        {"r2_food_ingredient_preferences","satisfaction_sum","REAL NOT NULL DEFAULT 0"},
        {"r2_food_ingredient_preferences","rating_count","INTEGER NOT NULL DEFAULT 0"},
        {"r2_food_ingredient_preferences","updated_at","INTEGER NOT NULL DEFAULT 0"},
        {"r2_food_preferences","satisfaction_sum","REAL NOT NULL DEFAULT 0"},
        {"r2_food_preferences","rating_count","INTEGER NOT NULL DEFAULT 0"},
        {"r2_food_preferences","updated_at","INTEGER NOT NULL DEFAULT 0"},

        {"r2_reality_self_facts","evidence","TEXT"},
        {"r2_reality_self_facts","updated_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_money_account","cash_cents","INTEGER NOT NULL DEFAULT 0"},
        {"r2_money_account","bank_cents","INTEGER NOT NULL DEFAULT 0"},
        {"r2_money_account","updated_at","TEXT NOT NULL DEFAULT ''"},

        {"r2_tv_state","power","INTEGER NOT NULL DEFAULT 0"},
        {"r2_tv_state","source_kind","TEXT NOT NULL DEFAULT 'input'"},
        {"r2_tv_state","source_value","INTEGER NOT NULL DEFAULT 1"},
        {"r2_tv_state","updated_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_tv_devices","connection_kind","TEXT NOT NULL DEFAULT 'input'"},
        {"r2_tv_devices","port","INTEGER NOT NULL DEFAULT 2"},
        {"r2_tv_devices","connected","INTEGER NOT NULL DEFAULT 1"},
        {"r2_tv_devices","updated_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_tv_vcr_tapes","position_seconds","REAL NOT NULL DEFAULT 0"},
        {"r2_tv_vcr_tapes","updated_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_tv_vcr_state","cassette_path","TEXT"},
        {"r2_tv_vcr_state","transport","TEXT NOT NULL DEFAULT 'stop'"},
        {"r2_tv_vcr_state","updated_at","TEXT NOT NULL DEFAULT ''"},

        {"r2_reality_containers","kind","TEXT NOT NULL DEFAULT 'container'"},
        {"r2_reality_containers","description","TEXT"},
        {"r2_reality_containers","parent","TEXT"},
        {"r2_reality_containers","created_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_reality_objects","description","TEXT"},
        {"r2_reality_objects","quantity","INTEGER NOT NULL DEFAULT 1"},
        {"r2_reality_objects","container","TEXT NOT NULL DEFAULT 'room'"},
        {"r2_reality_objects","owner","TEXT NOT NULL DEFAULT 'R2'"},
        {"r2_reality_objects","created_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_reality_objects","updated_at","TEXT NOT NULL DEFAULT ''"},
        {"r2_reality_ticks","elapsed_seconds","INTEGER NOT NULL DEFAULT 0"},
        {"r2_reality_ticks","created_at","TEXT NOT NULL DEFAULT ''"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (reality_ensure_existing_column(columns[i][0], columns[i][1],
                                           columns[i][2]) != 0)
            return -1;
    }

    /* The current schema permits only one connected device per physical port.
       Older DBs did not always enforce that rule. Keep every device row, but
       mark later collisions disconnected before the new unique index is made. */
    if (reality_table_exists("r2_tv_devices") &&
        exec_sql(
            "UPDATE r2_tv_devices SET connected=0 "
            "WHERE connected=1 AND rowid NOT IN ("
            "SELECT MIN(rowid) FROM r2_tv_devices WHERE connected=1 "
            "GROUP BY connection_kind,port);") != 0)
        return -1;

    return 0;
}

static int backfill_reality_timestamps(void)
{
    /* This runs after CREATE TABLE statements, so fresh and upgraded tables
       both exist. Never overwrite a non-empty timestamp or modeled state. */
    return exec_sql(
        "UPDATE r2_reality_item_memory SET collected_at=CAST(strftime('%s','now') AS INTEGER) WHERE collected_at IS NULL OR collected_at=0;"
        "UPDATE r2_reality_item_memory SET last_decay_at=collected_at WHERE last_decay_at IS NULL OR last_decay_at=0;"
        "UPDATE r2_food_ingredient_preferences SET updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE updated_at IS NULL OR updated_at=0;"
        "UPDATE r2_food_preferences SET updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE updated_at IS NULL OR updated_at=0;"
        "UPDATE r2_reality_self_facts SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_money_account SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_tv_state SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_tv_devices SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_tv_vcr_tapes SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_tv_vcr_state SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_reality_containers SET created_at=CURRENT_TIMESTAMP WHERE created_at IS NULL OR created_at='';"
        "UPDATE r2_reality_objects SET created_at=CURRENT_TIMESTAMP WHERE created_at IS NULL OR created_at='';"
        "UPDATE r2_reality_objects SET updated_at=CURRENT_TIMESTAMP WHERE updated_at IS NULL OR updated_at='';"
        "UPDATE r2_reality_ticks SET created_at=CURRENT_TIMESTAMP WHERE created_at IS NULL OR created_at='';");
}

static int reality_ensure_self_column(const char *column, const char *definition)
{
    if (reality_column_exists("r2_reality_self", column)) return 0;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "ALTER TABLE r2_reality_self ADD COLUMN %s %s;",
                     column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
    return exec_sql(sql);
}

static int migrate_reality_self_schema(void)
{
    const char *columns[][2] = {
        {"hunger", "REAL NOT NULL DEFAULT 0"},
        {"satisfaction", "REAL NOT NULL DEFAULT 100"},
        {"seconds_since_meal", "REAL NOT NULL DEFAULT 0"},
        {"sleepiness", "REAL NOT NULL DEFAULT 0"},
        {"energy", "REAL NOT NULL DEFAULT 100"},
        {"last_tick", "INTEGER NOT NULL DEFAULT 0"},
        {"updated_at", "TEXT NOT NULL DEFAULT ''"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (reality_ensure_self_column(columns[i][0], columns[i][1]) != 0)
            return -1;
    }
    if (exec_sql(
        "INSERT OR IGNORE INTO r2_reality_self"
        "(id,hunger,satisfaction,seconds_since_meal,sleepiness,energy,last_tick) "
        "VALUES(1,0,100,0,0,100,strftime('%s','now'));"
        "UPDATE r2_reality_self SET updated_at=CURRENT_TIMESTAMP "
        "WHERE updated_at IS NULL OR updated_at='';") != 0)
        return -1;
    return 0;
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
    if (migrate_existing_reality_schema() != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate existing Reality-owned tables before schema setup.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
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
        "CREATE TABLE IF NOT EXISTS r2_reality_self_facts (key TEXT PRIMARY KEY, value TEXT NOT NULL, evidence TEXT, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_money_account (id INTEGER PRIMARY KEY CHECK(id=1),cash_cents INTEGER NOT NULL DEFAULT 0 CHECK(cash_cents>=0),bank_cents INTEGER NOT NULL DEFAULT 0 CHECK(bank_cents>=0),updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO r2_money_account(id,cash_cents,bank_cents) VALUES(1,0,0);"
        "CREATE TABLE IF NOT EXISTS r2_money_transfer_journal (id INTEGER PRIMARY KEY CHECK(id=1),old_cash_cents INTEGER NOT NULL,old_bank_cents INTEGER NOT NULL,new_cash_cents INTEGER NOT NULL,new_bank_cents INTEGER NOT NULL,operation TEXT NOT NULL,phase TEXT NOT NULL CHECK(phase IN ('pending','committed')),created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS r2_purchase_journal (id INTEGER PRIMARY KEY CHECK(id=1),old_cash_cents INTEGER NOT NULL,old_bank_cents INTEGER NOT NULL,new_cash_cents INTEGER NOT NULL,new_bank_cents INTEGER NOT NULL,item_name TEXT NOT NULL,item_description TEXT NOT NULL,destination TEXT NOT NULL,pre_quantity INTEGER NOT NULL,pre_container TEXT NOT NULL,phase TEXT NOT NULL CHECK(phase IN ('pending','committed')),created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
         "CREATE TABLE IF NOT EXISTS r2_inventory_transfer_journal (id INTEGER PRIMARY KEY CHECK(id=1),operation TEXT NOT NULL,item_name TEXT NOT NULL COLLATE NOCASE,reality_exists INTEGER NOT NULL,reality_description TEXT NOT NULL,reality_quantity INTEGER NOT NULL,reality_container TEXT NOT NULL,reality_owner TEXT NOT NULL,item_memory_max_id INTEGER NOT NULL,fridge_exists INTEGER NOT NULL,fridge_description TEXT NOT NULL,fridge_quantity INTEGER NOT NULL,fridge_fullness REAL NOT NULL,fridge_energy REAL NOT NULL,fridge_ingredients TEXT NOT NULL,fridge_taste TEXT NOT NULL,phase TEXT NOT NULL CHECK(phase IN ('pending','committed')),created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
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
        "INSERT OR IGNORE INTO r2_reality_objects(name,description,quantity,container,owner) "
        "VALUES('TV','CRT television with a built-in VCR and expandable external inputs.',1,'room','R2');";
    if (exec_sql(schema) != 0) {
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (backfill_reality_timestamps() != 0) {
        fprintf(stderr, "[R2 Reality] Could not backfill timestamps for upgraded tables.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    /* Upgrade old persistent self-state schemas in place. Missing fields are
       added with neutral defaults; existing hunger/energy/need values survive. */
    if (migrate_reality_self_schema() != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate persistent need-state schema.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (migrate_legacy_reality() != 0) {
        fprintf(stderr, "[R2 Reality] Legacy migration failed; refusing to discard old state.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    /* Convert the old increasing-hunger scale to the intended decreasing
       fullness scale once, after importing any legacy Reality database. */
    if (exec_sql(
        "INSERT OR IGNORE INTO r2_reality_meta(key,value) VALUES('hunger_fullness_semantics_v2','no');"
        "UPDATE r2_reality_self SET hunger=MIN(100,MAX(0,100-hunger)) "
        "WHERE (SELECT value FROM r2_reality_meta WHERE key='hunger_fullness_semantics_v2')='no';"
        "UPDATE r2_reality_meta SET value='yes' WHERE key='hunger_fullness_semantics_v2' AND value='no';"
    ) != 0) {
        fprintf(stderr, "[R2 Reality] Could not migrate hunger/fullness semantics.\n");
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
        "DELETE FROM r2_reality_containers WHERE lower(name)='toy box';"
        "INSERT OR IGNORE INTO r2_reality_objects(name,description,quantity,container,owner) "
        "VALUES('TV','CRT television with a built-in VCR and expandable external inputs.',1,'room','R2');") != 0) {
        fprintf(stderr, "[R2 Reality] Could not normalize persistent container hierarchy.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (remove_legacy_phantom_tracked_food() != 0) {
        fprintf(stderr, "[R2 Reality] Could not remove legacy phantom-food copies safely.\n");
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
    if (money_initialize_files(cash_seeded) != 0) {
        fprintf(stderr, "[R2 Reality] Could not initialize authoritative wallet files.\n");
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    if (fridge_init() != 0) {
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    fridge_sync_mirrors();
    /* Restore both sides of any interrupted fridge/pocket transfer before exposing inventory. */
    if (inventory_transfer_recover_locked() != 0) {
        fprintf(stderr, "[R2 Reality] Pending inventory transfer recovery failed; refusing to start with ambiguous ownership.\n");
        r2_fridge_shutdown();
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }
    /* Resolve any purchase interrupted between physical payment and inventory persistence. */
    if (purchase_journal_recover() != 0) {
        fprintf(stderr, "[R2 Reality] Pending purchase recovery failed; refusing to start with ambiguous ownership.\\n");
        r2_fridge_shutdown();
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock); return -1;
    }

    /* Catch up persistent world time as one checked transaction. */
    sqlite3_stmt *st = NULL;
    sqlite3_int64 last = 0;
    int catchup_rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (catchup_rc == SQLITE_OK)
        catchup_rc = sqlite3_prepare_v2(reality_db,
            "SELECT last_tick FROM r2_reality_self WHERE id=1", -1, &st, NULL);
    if (catchup_rc == SQLITE_OK) {
        int step = sqlite3_step(st);
        if (step == SQLITE_ROW) last = sqlite3_column_int64(st, 0);
        else catchup_rc = step == SQLITE_DONE ? SQLITE_NOTFOUND : step;
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    /* The private clock is a fixed offset from host time and survives relaunches.
       Long downtime is real modeled elapsed time; do not silently discard it
       just because it exceeds a week. */
    if (catchup_rc == SQLITE_OK && last > 0 && now > last) {
        if (update_hunger_locked((double)(now - last)) != 0) catchup_rc = SQLITE_ERROR;
        if (catchup_rc == SQLITE_OK) {
            catchup_rc = sqlite3_prepare_v2(reality_db,
                "INSERT INTO r2_reality_ticks(previous_tick,current_tick,elapsed_seconds) VALUES(?,?,?)",
                -1, &st, NULL);
            if (catchup_rc == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, last);
                sqlite3_bind_int64(st, 2, now);
                sqlite3_bind_int64(st, 3, now - last);
                catchup_rc = sqlite3_step(st);
            }
            if (st) sqlite3_finalize(st);
            st = NULL;
        }
        if (catchup_rc == SQLITE_DONE) catchup_rc = SQLITE_OK;
    }
    if (catchup_rc == SQLITE_OK) {
        catchup_rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_self SET last_tick=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
            -1, &st, NULL);
        if (catchup_rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            catchup_rc = sqlite3_step(st);
            if (catchup_rc == SQLITE_DONE && sqlite3_changes(reality_db) != 1)
                catchup_rc = SQLITE_NOTFOUND;
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
        if (catchup_rc == SQLITE_DONE) catchup_rc = SQLITE_OK;
    }
    if (catchup_rc == SQLITE_OK)
        catchup_rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (catchup_rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        fprintf(stderr, "[R2 Reality] Persistent time catch-up failed; refusing partial startup state.\n");
        if (fridge_db) { sqlite3_close(fridge_db); fridge_db = NULL; }
        sqlite3_close(reality_db); reality_db = NULL;
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
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
    /* Wait for any purchase to finish its cross-database commit before
       closing either Reality or fridge handles. Lock order matches mutators. */
    pthread_mutex_lock(&purchase_lock);
    pthread_mutex_lock(&reality_lock);
    if (reality_db) {
        sqlite3_wal_checkpoint_v2(reality_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        sqlite3_close(reality_db);
        reality_db = NULL;
    }
    r2_fridge_shutdown();
    reality_ready = 0;
    pthread_mutex_unlock(&reality_lock);
    pthread_mutex_unlock(&purchase_lock);
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
                "Hunger is near its maximum modeled level. This is a simulation state, not a claim of biological metabolism.",
                1, 0);
        }
    }
}

int r2_reality_tick(void)
{
    if (!r2_reality_is_initialized()) return -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL), last = 0;
    pthread_mutex_lock(&reality_lock);
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(reality_db, "SELECT last_tick FROM r2_reality_self WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int step = sqlite3_step(st);
        if (step == SQLITE_ROW) last = sqlite3_column_int64(st, 0);
        else rc = step == SQLITE_DONE ? SQLITE_NOTFOUND : step;
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    /* Match startup catch-up: a long but valid forward interval still advances
       needs, world time, and the 72-hour starvation marker. */
    if (rc == SQLITE_OK && last > 0 && now > last) {
        rc = update_hunger_locked((double)(now - last)) == 0 ? SQLITE_OK : SQLITE_ERROR;
        if (rc == SQLITE_OK) {
            rc = sqlite3_prepare_v2(reality_db,
                "INSERT INTO r2_reality_ticks(previous_tick,current_tick,elapsed_seconds) VALUES(?,?,?)",
                -1, &st, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, last);
                sqlite3_bind_int64(st, 2, now);
                sqlite3_bind_int64(st, 3, now - last);
                rc = sqlite3_step(st);
            }
            if (st) sqlite3_finalize(st);
            st = NULL;
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_item_memory SET precision='vague', exact_quantity=NULL, approximate_quantity=NULL, last_decay_at=? WHERE precision!='vague' AND collected_at<=?",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            sqlite3_bind_int64(st, 2, now - 30 * 86400);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_item_memory SET precision='approximate', approximate_quantity=MAX(1,CAST((exact_quantity+2)/5 AS INTEGER)*5), exact_quantity=NULL, last_decay_at=? WHERE precision='exact' AND collected_at<=?",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            sqlite3_bind_int64(st, 2, now - 7 * 86400);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_self SET last_tick=?, updated_at=CURRENT_TIMESTAMP WHERE id=1",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, now);
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE && sqlite3_changes(reality_db) != 1) rc = SQLITE_NOTFOUND;
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&reality_lock);
    if (rc == SQLITE_OK) maybe_record_starvation();
    return rc == SQLITE_OK ? 0 : -1;
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
    const char *hstate = hunger < 10 ? "starving" : hunger < 25 ? "very hungry" : hunger < 50 ? "hungry" : hunger < 75 ? "getting hungry" : "satisfied";
    snprintf(out,4096,"SELF CONTINUITY\nSatisfaction: %5.1f/150\nHunger: %04.1f/100 (%s)\nTime since meal: %.1f hours\nSleepiness: %.1f/100\nEnergy: %.1f/100\nModeled world time elapsed: %lld days, %lld hours\nObjects tracked in the world: %d\nFullness (the 0..100 hunger meter) decreases steadily to zero over roughly three modeled hours without food; eating raises it according to the configured food fullness value. Satisfaction is a separate 0..150 measure. Sleepiness rises 12.5/hour and energy falls 10/hour while awake or asleep; sleep then restores energy and reduces sleepiness.\nAfter 72 hours without food, prolonged starvation is recorded; needs persist across restarts.\n",
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
    char *facts = query_text("SELECT key,value,evidence FROM r2_reality_self_facts ORDER BY updated_at DESC LIMIT 40", NULL);
    char *items_memory = query_text(
        "WITH detailed AS (SELECT item_name,CASE precision WHEN 'exact' THEN printf('remembers collecting exactly %d',exact_quantity) ELSE printf('remembers collecting about %d',approximate_quantity) END AS memory,CASE precision WHEN 'exact' THEN printf('%d days ago',MAX(0,(strftime('%s','now')-collected_at)/86400)) ELSE printf('about %d weeks ago',MAX(1,ROUND((strftime('%s','now')-collected_at)/604800.0))) END AS timing,collected_at FROM r2_reality_item_memory WHERE precision IN ('exact','approximate')), vague AS (SELECT item_name,'remembers collecting some; exact quantity and timing have faded' AS memory,'older vague memory' AS timing,MAX(collected_at) AS collected_at FROM r2_reality_item_memory WHERE precision='vague' GROUP BY item_name) SELECT item_name,memory,timing FROM (SELECT * FROM detailed UNION ALL SELECT * FROM vague) ORDER BY collected_at DESC LIMIT 80", NULL);
    char *food_preferences = query_text("SELECT ingredient,printf('average enjoyment rating %.2f/2',satisfaction_sum/rating_count),printf('%d ratings',rating_count) FROM r2_food_ingredient_preferences WHERE rating_count>0 ORDER BY satisfaction_sum*1.0/rating_count DESC LIMIT 24", NULL);
    char *food_experiences = query_text("SELECT food_name,printf('enjoyment rating %+d/2',satisfaction),notes FROM r2_food_experiences WHERE satisfaction IS NOT NULL ORDER BY eaten_at DESC LIMIT 20", NULL);
    char *foods = food_metrics_context();
    char *fridge=r2_fridge_context(); char *age=origin_age_context(); char *money=r2_reality_money_context();
    char *recent_log=r2_log_recent(5);
    char *visual_memory=r2_visual_recent(2);
    /* Keep the combined context usable when one optional subsystem query
       fails. Each fallback explicitly marks the missing evidence so the model
       cannot mistake unavailable state for an empty inventory or known fact. */
    if(!status) status=strdup("CURRENT SELF-STATE UNAVAILABLE: the live needs/status query failed. Do not infer hunger, sleepiness, energy, or elapsed time.");
    if(!room) room=strdup("ROOM AND INVENTORY STATE UNAVAILABLE: the current inventory query failed. Do not infer what objects exist or are accessible, and do not take inventory-dependent actions.");
    if(!facts) facts=strdup("Self-continuity facts are unavailable because their database query failed.");
    if(!items_memory) items_memory=strdup("Collection memories are unavailable because their database query failed; do not infer past collection history.");
    if(!food_preferences) food_preferences=strdup("Learned food and ingredient preferences are unavailable because their query failed.");
    if(!food_experiences) food_experiences=strdup("Food-experience history unavailable: the query failed. Do not infer a rating or eating experience.");
    if(!foods) foods=strdup("Food metrics unavailable: do not guess fullness, energy, ingredients, or taste.");
    if(!fridge) fridge=strdup("FRIDGE STATE UNAVAILABLE: the fridge database/query failed. Do not assume stock exists or attempt a fridge-eat action until stock can be verified.");
    if(!age) age=strdup("Age evidence is unavailable; do not guess age.");
    if(!money) money=strdup("MONEY STATE UNAVAILABLE: the wallet/bank query failed. Do not claim balances or perform a transaction based on an assumed balance.");
    if(!recent_log) recent_log=strdup("Recent Life Log evidence is unavailable.");
    if(!visual_memory) visual_memory=strdup("No stored visual observations are available.");
    if(!status||!room||!facts||!items_memory||!food_preferences||!food_experiences||!foods||!fridge||!age||!money||!recent_log||!visual_memory){
        free(status);free(room);free(fridge);free(facts);free(items_memory);free(food_preferences);free(food_experiences);free(foods);free(age);free(money);free(recent_log);free(visual_memory);return NULL;}
    size_t n=strlen(status)+strlen(room)+strlen(fridge)+strlen(facts)+strlen(items_memory)+strlen(food_preferences)+strlen(food_experiences)+strlen(foods)+strlen(age)+strlen(money)+strlen(recent_log)+strlen(visual_memory)+6000;
    char *out=malloc(n);
    if(out)snprintf(out,n,
        "PERSISTENT REALITY CONTEXT (authoritative database state; do not invent changes):\n"
        "%s\n%s\n%s\n%s\n%s\nRECENT LIFE LOG EVIDENCE (events and observations; not automatically current):\n%s\nRECENT VISUAL EXPERIENCES (actual stored observations; not necessarily current):\n%s\nRECENT SELF-CONTINUITY FACTS (up to 40 newest; older facts remain stored):\n%s\nCOLLECTION MEMORIES (precision intentionally fades; not current inventory):\n%s\nLEARNED FOOD/INGREDIENT PREFERENCES (top 24 by average rating; full history remains stored):\n%s\nRECENT RATED FOOD EXPERIENCES:\n%s\nAVAILABLE FOOD METRICS:\n%s\n"
        "WORLD ACTIONS: Put one action on its own line. Use [WORLD] look to inspect the room; "
        "[WORLD] add|name|description|container|quantity to collect/add a stack (adds to an existing stack and records a collection memory); "
        "[WORLD] move|name|container to relocate an existing item without counting a new collection; [WORLD] remove|name to remove it; "
        "[WORLD] eat|food|auto to update hunger using the authoritative XML metric; "
        "[WORLD] sleep|hours to advance sleep recovery and trigger a private dream simulation; "
        "[WORLD] dream|description to record a reported dream; "
        "[WORLD] ratefood|food|-2..2|reason to rate the most recent unrated eating experience. "
        "[WORLD] self|key|value|evidence to record a self-state fact. "
        "[WORLD] fridge|look to inspect fridge stock; [WORLD] fridge_take|food to move one item into pockets; [WORLD] fridge_eat|food to eat directly from fridge stock without consuming a similarly named pocket item; [WORLD] fridge_store|food to move a tracked inventory item into fridge stock. "
        "The fridge is physically at home, outside the room (a sibling of room/ and pockets/), and is accessible only while R2 is home. Its stock is stored in a separate r2_fridge.db; an empty fridge stays empty until food is explicitly added or stored. Never infer that food exists merely because R2 is hungry. "
        "Food metrics live in room/food_metrics.xml; each food can define fullness, energy, ingredients (comma-separated), and taste (sensory description). Use only listed metrics and auto rather than guessing. "
        "Containers: room, shelf, and one storage box; pockets are portable anywhere, the fridge is a fixed home object, and the wallet is inside pockets. While away from home, R2 may access only pockets and wallet; room objects and fridge stock require returning home. The old toy box is the same box and is normalized into it. Named containers can be created by moving an item to a new container name. "
        "Food fullness points are modeled values, not measured biological facts. The hunger/fullness meter drains over about three hours without food and eating raises it. Sleep advances needs and world time. "
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
    if(!name || !*name || strlen(name)>REALITY_MAX_TEXT ||
       (description && strlen(description)>REALITY_MAX_TEXT) || quantity < 1)
        return -1;
    char canonical_container[REALITY_MAX_TEXT + 1];
    canonical_container_name(container, canonical_container, sizeof(canonical_container));
    container = canonical_container;
    if (!strcasecmp(container, "fridge")) {
        if (!r2_reality_is_initialized() || !reality_is_home()) return -1;
        char ingredients[1024] = {0}, taste[1024] = {0};
        double fullness = 0.0, energy = 0.0;
        if (food_metric(name, &fullness, &energy) != 0) return -1;
        food_attributes(name, ingredients, sizeof(ingredients), taste, sizeof(taste));
        return r2_fridge_add_item(name, description, quantity, fullness, energy, ingredients, taste);
    }
    if(!r2_reality_is_initialized() || !container_accessible(container)) return -1;
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    int total_quantity = quantity;
    pthread_mutex_lock(&reality_lock);
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }

    sqlite3_int64 prior_quantity = 0;
    sqlite3_stmt *prior = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "SELECT container,quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
        -1, &prior, NULL);
    if (rc == SQLITE_OK) {
        bind_text(prior, 1, name);
        rc = sqlite3_step(prior);
        if (rc == SQLITE_ROW) {
            const unsigned char *oc = sqlite3_column_text(prior, 0);
            if (oc) snprintf(old_container, sizeof(old_container), "%s", (const char *)oc);
            prior_quantity = sqlite3_column_int64(prior, 1);
            rc = SQLITE_DONE;
        }
    }
    if (prior) sqlite3_finalize(prior);
    if (rc != SQLITE_DONE) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    if (prior_quantity < 0 || prior_quantity > (sqlite3_int64)INT_MAX - quantity) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    if (*old_container && !container_accessible(old_container)) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    /* Object names identify a stack in the canonical Reality database. An
       add/collect operation may increase that stack only where it already is;
       silently changing its container would move every existing unit when
       collecting one more unit. Moving a stack is an explicit move action. */
    if (*old_container && strcasecmp(old_container, container) != 0) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        fprintf(stderr,
                "[R2 Reality] Refusing to add '%s' in '%s': the existing stack is in '%s'. "
                "Use an explicit move or a distinct item name.\n",
                name, container, old_container);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, container);
        bind_text(st, 2, container_is_inventory(container) ? "inventory" : "container");
        bind_text(st, 3, "Persistent object container");
        bind_text(st, 4, container_parent_name(container));
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;

    if (rc == SQLITE_DONE)
        rc = sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_objects(name,description,quantity,container,owner) VALUES(?,?,?,?, 'R2') "
            "ON CONFLICT(name) DO UPDATE SET description=excluded.description,quantity=r2_reality_objects.quantity+excluded.quantity,container=excluded.container,updated_at=CURRENT_TIMESTAMP",
            -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name);
        bind_text(st, 2, description);
        sqlite3_bind_int(st, 3, quantity);
        bind_text(st, 4, container);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;

    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_item_memory(item_name,description,exact_quantity,precision,collected_at,last_decay_at) VALUES(?,?,?,'exact',?,?)",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_int64 collected = (sqlite3_int64)time(NULL);
            bind_text(st, 1, name);
            bind_text(st, 2, description);
            sqlite3_bind_int(st, 3, quantity);
            sqlite3_bind_int64(st, 4, collected);
            sqlite3_bind_int64(st, 5, collected);
            rc = sqlite3_step(st);
        }
        if (st) sqlite3_finalize(st);
        st = NULL;
    }

    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(reality_db,
            "SELECT quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            bind_text(st, 1, name);
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                sqlite3_int64 stored_quantity = sqlite3_column_int64(st, 0);
                if (stored_quantity < 1 || stored_quantity > INT_MAX)
                    rc = SQLITE_TOOBIG;
                else {
                    total_quantity = (int)stored_quantity;
                    rc = SQLITE_DONE;
                }
            }
        }
        if (st) sqlite3_finalize(st);
    }

    if (rc == SQLITE_DONE) {
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    } else {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_OK) return -1;
    if (*old_container) mirror_remove(name, old_container);
    if (mirror_write(name, description, total_quantity, container) != 0)
        fprintf(stderr, "[R2 Reality] Database saved, but room mirror file could not be updated for '%s'.\n", name);
    char summary[512],details[2048];
    snprintf(summary,sizeof(summary),"R2 recorded item '%.200s' in %.200s.",name,container);
    snprintf(details,sizeof(details),"Item=%.300s; description=%.1200s; container=%.200s; quantity=%d",name,description?description:"",container,quantity);
    bridge_event("object_added",summary,details,1,1);
    return 0;
}

static int reality_move_item_to_fridge(const char *name)
{
    if (!name || !*name || !r2_reality_is_initialized() || !reality_is_home() ||
        !container_accessible("fridge"))
        return -1;

    double fullness = 0.0, energy = 0.0;
    char ingredients[1024] = {0}, taste[1024] = {0};
    if (food_metric(name, &fullness, &energy) != 0) return -1;
    food_attributes(name, ingredients, sizeof(ingredients), taste, sizeof(taste));

    InventoryTransferSnapshot snapshot;
    pthread_mutex_lock(&reality_lock);
    int rc = inventory_transfer_recover_locked();
    if (rc == 0) rc = inventory_transfer_snapshot_locked(name, &snapshot);
    if (rc == 0 && (!snapshot.reality_exists || snapshot.reality_quantity < 1 ||
                    !*snapshot.reality_container ||
                    !strcasecmp(snapshot.reality_container, "fridge") ||
                    !container_accessible(snapshot.reality_container)))
        rc = -1;
    if (rc == 0)
        rc = inventory_transfer_journal_begin_locked("move_to_fridge", &snapshot);
    pthread_mutex_unlock(&reality_lock);
    if (rc != 0) return -1;

    if (r2_fridge_add_item(name, snapshot.reality_description,
                           snapshot.reality_quantity, fullness, energy,
                           ingredients, taste) != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Reality] CRITICAL: failed fridge-add rollback for '%s' is pending recovery.\n", name);
        return -1;
    }

#ifdef R2_INVENTORY_TEST_FAILURE_INJECTION
    if (getenv("R2_TEST_INVENTORY_CRASH_AFTER_FRIDGE_ADD")) {
        /* Leave the pending journal exactly as a process crash would. */
        return -1;
    }
#endif

    if (r2_reality_remove_item(name) != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Reality] CRITICAL: failed move rollback for '%s' is pending recovery.\n", name);
        return -1;
    }
    if (inventory_transfer_journal_mark_committed() != 0) {
        if (inventory_transfer_recover() != 0)
            fprintf(stderr, "[R2 Reality] CRITICAL: move commit recovery for '%s' is pending.\n", name);
        return -1;
    }
    if (inventory_transfer_journal_clear() != 0)
        fprintf(stderr, "[R2 Reality] Committed transfer journal retained for startup cleanup ('%s').\n", name);

    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 stored %s in the fridge.", name);
    snprintf(details, sizeof(details),
             "Object=%.300s; source container=%.200s; quantity=%d; destination=separate fridge database.",
             name, snapshot.reality_container, snapshot.reality_quantity);
    bridge_event("fridge_item_stored", summary, details, 1, 0);
    return 0;
}

int r2_reality_move_item(const char *name,const char *container)
{
    if(!name||!*name||!container||!*container||strlen(name)>REALITY_MAX_TEXT||strlen(container)>REALITY_MAX_TEXT||!r2_reality_is_initialized()) return -1;
    char canonical_container[REALITY_MAX_TEXT + 1];
    canonical_container_name(container, canonical_container, sizeof(canonical_container));
    container = canonical_container;
    if (!strcasecmp(container, "fridge")) {
        pthread_mutex_lock(&purchase_lock);
        int transfer_rc = reality_move_item_to_fridge(name);
        pthread_mutex_unlock(&purchase_lock);
        return transfer_rc;
    }
    char old_container[REALITY_MAX_TEXT + 1] = {0};
    char old_description[REALITY_MAX_TEXT + 1] = {0};
    int old_quantity = 1;
    pthread_mutex_lock(&reality_lock);
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }

    sqlite3_stmt *prior = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "SELECT container,description,quantity FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
        -1, &prior, NULL);
    if (rc == SQLITE_OK) {
        bind_text(prior, 1, name);
        rc = sqlite3_step(prior);
        if (rc == SQLITE_ROW) {
            const unsigned char *oc = sqlite3_column_text(prior, 0);
            const unsigned char *od = sqlite3_column_text(prior, 1);
            if (oc) snprintf(old_container, sizeof(old_container), "%s", (const char *)oc);
            if (od) snprintf(old_description, sizeof(old_description), "%s", (const char *)od);
            old_quantity = sqlite3_column_int(prior, 2);
            rc = SQLITE_DONE;
        } else if (rc == SQLITE_DONE) {
            rc = SQLITE_NOTFOUND;
        }
    }
    if (prior) sqlite3_finalize(prior);
    if (rc != SQLITE_DONE || !*old_container ||
        !container_accessible(old_container) || !container_accessible(container)) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_reality_containers(name,kind,description,parent) VALUES(?,?,?,?) ON CONFLICT(name) DO NOTHING",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, container);
        bind_text(st, 2, container_is_inventory(container) ? "inventory" : "container");
        bind_text(st, 3, "Persistent object container");
        bind_text(st, 4, container_parent_name(container));
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    st = NULL;

    if (rc == SQLITE_DONE)
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_objects SET container=?,updated_at=CURRENT_TIMESTAMP WHERE name=? COLLATE NOCASE",
            -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, container);
        bind_text(st, 2, name);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 0)
            rc = SQLITE_NOTFOUND;
    }
    if (st) sqlite3_finalize(st);

    if (rc == SQLITE_DONE) {
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    } else {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_OK) return -1;
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
    if (consume_tracked_item) {
        /* Tracked inventory must use the configured authoritative metric.
           Never let a model-supplied numeric override turn an unknown object
           or a zero-fullness item into a meal. */
        if (food_metric(food, &fullness, &energy_bonus) != 0) {
            fprintf(stderr, "[R2 Reality] No food metric found for '%s' in room/food_metrics.xml.\n", food);
            return -1;
        }
    } else if (fullness >= 0.0) {
        if (!isfinite(fullness)) return -1;
        if (fullness > 100.0) fullness = 100.0;
        energy_bonus = fullness * 0.1;
    } else {
        if (food_metric(food, &fullness, &energy_bonus) != 0) {
            fprintf(stderr, "[R2 Reality] No food metric found for '%s' in room/food_metrics.xml.\n", food);
            return -1;
        }
    }
    /* A zero-fullness item is not a meal: it must not reset the meal clock
       or clear the prolonged-starvation marker without restoring fullness. */
    if (!isfinite(fullness) || fullness <= 0.0) return -1;
    if (energy_override >= 0.0) energy_bonus = energy_override;
    if (fullness > 100.0) fullness = 100.0;
    if (!isfinite(energy_bonus)) return -1;
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
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
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
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }

    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_reality_self SET hunger=MIN(100,hunger+?), satisfaction=MIN(150,satisfaction+?), seconds_since_meal=0, energy=MIN(100,energy+?), updated_at=CURRENT_TIMESTAMP WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_double(st, 1, fullness);
        sqlite3_bind_double(st, 2, fullness);
        sqlite3_bind_double(st, 3, energy_bonus);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(reality_db) != 1)
            rc = SQLITE_NOTFOUND;
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
                if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 1)
                    consumed_tracked_item = 1;
                else if (rc == SQLITE_DONE)
                    rc = SQLITE_NOTFOUND;
            } else rc = SQLITE_ERROR;
        } else {
            if (sqlite3_prepare_v2(reality_db,
                "DELETE FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
                -1, &st, NULL) == SQLITE_OK) {
                bind_text(st, 1, food);
                rc = sqlite3_step(st);
                if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 1)
                    consumed_tracked_item = 1;
                else if (rc == SQLITE_DONE)
                    rc = SQLITE_NOTFOUND;
            } else rc = SQLITE_ERROR;
        }
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        st = NULL;
        if (sqlite3_prepare_v2(reality_db,
            "INSERT INTO r2_reality_self_facts(key,value,evidence) VALUES('starvation_72h_logged','no','Reset by qualifying food intake') ON CONFLICT(key) DO UPDATE SET value='no',evidence='Reset by qualifying food intake',updated_at=CURRENT_TIMESTAMP",
            -1, &st, NULL) == SQLITE_OK)
            rc = sqlite3_step(st);
        else
            rc = SQLITE_ERROR;
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
    if (rc == SQLITE_DONE) {
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    } else {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_DONE && rc != SQLITE_OK) return -1;
    /* COMMIT returns SQLITE_OK; all earlier statements return SQLITE_DONE. */

    if (consumed_tracked_item) {
        if (quantity == 1) mirror_remove(food, container);
        else if (mirror_write(food, description, quantity - 1, container) != 0)
            fprintf(stderr, "[R2 Reality] Food state saved, but mirror could not be updated for '%s'.\n", food);
    }

    char summary[512], details[1024];
    snprintf(summary, sizeof(summary), "R2 ate %s; fullness increased by the food's %.1f-point value.", food, fullness);
    snprintf(details, sizeof(details), "Food=%s; modeled fullness increase=%.1f points (maximum 100); satisfaction increase=%.1f points (maximum 150); tracked object consumed=%s; these are simulated need values, not measured biological quantities.",
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
        "Food consumption completed and modeled fullness/satisfaction state was updated.", 0);
    return 0;
}


int r2_reality_eat(const char *food, double fullness)
{
    return reality_eat_internal(food, fullness, 1, -1.0);
}

/* Model output may fail to choose an obvious meal. Feed only when the
 * fullness meter is low, using authoritative metrics from accessible inventory
 * or the fridge at home. Never invent food or bypass location. */
int r2_reality_autonomous_feed_if_needed(void)
{
    if (!r2_reality_is_initialized()) return -1;
    if (r2_reality_tick() != 0) return -1;

    char food[512] = {0};
    double hunger = 0.0;
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT hunger FROM r2_reality_self WHERE id=1", -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        hunger = sqlite3_column_double(st, 0);
    else rc = SQLITE_ERROR;
    if (st) sqlite3_finalize(st);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&reality_lock);
        return -1;
    }
    if (hunger >= 50.0) {
        pthread_mutex_unlock(&reality_lock);
        return 0;
    }

    st = NULL;
    rc = sqlite3_prepare_v2(reality_db,
        "SELECT name,container FROM r2_reality_objects WHERE quantity>0 ORDER BY id",
        -1, &st, NULL);
    int step_rc = SQLITE_OK;
    if (rc == SQLITE_OK) {
        while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 0);
            const unsigned char *container = sqlite3_column_text(st, 1);
            if (!name || !container || !container_accessible((const char *)container))
                continue;
            double candidate_fullness = 0.0;
            if (food_metric((const char *)name, &candidate_fullness, NULL) == 0 &&
                isfinite(candidate_fullness) && candidate_fullness > 0.0) {
                /* A known but zero-fullness metric is not a useful meal and
                   must not consume stock or prevent trying another food. */
                snprintf(food, sizeof(food), "%s", (const char *)name);
                break;
            }
        }
        if (!*food && step_rc != SQLITE_DONE) rc = step_rc;
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_OK) return -1;
    if (*food && reality_eat_internal(food, -1.0, 1, -1.0) == 0)
        return 1;

    if (!reality_is_home()) return 0;
    food[0] = '\0';
    pthread_mutex_lock(&fridge_lock);
    if (fridge_db) {
        st = NULL;
        if (sqlite3_prepare_v2(fridge_db,
            "SELECT name FROM r2_fridge_items WHERE quantity>0 AND fullness>0 ORDER BY name LIMIT 1",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 0);
            if (name) snprintf(food, sizeof(food), "%s", (const char *)name);
        }
        if (st) sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&fridge_lock);
    if (*food && r2_reality_fridge_eat(food, -1.0) == 0)
        return 1;
    return 0;
}

/* Read-only availability check used to validate autonomous sleep decisions.
 * Returns 1 when an accessible, metric-backed meal exists, 0 when none exists,
 * and -1 when authoritative inventory could not be checked safely. */
int r2_reality_autonomous_food_available(void)
{
    if (!r2_reality_is_initialized()) return -1;
    char food[512] = {0};
    pthread_mutex_lock(&reality_lock);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT name,container FROM r2_reality_objects WHERE quantity>0 ORDER BY id",
        -1, &st, NULL);
    int step_rc = SQLITE_OK;
    if (rc == SQLITE_OK) {
        while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 0);
            const unsigned char *container = sqlite3_column_text(st, 1);
            if (!name || !container || !container_accessible((const char *)container))
                continue;
            double fullness = 0.0;
            if (food_metric((const char *)name, &fullness, NULL) == 0 &&
                isfinite(fullness) && fullness > 0.0) {
                snprintf(food, sizeof(food), "%s", (const char *)name);
                break;
            }
        }
        if (!*food && step_rc != SQLITE_DONE) rc = step_rc;
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_OK) return -1;
    if (*food) return 1;
    if (!reality_is_home()) return 0;

    int available = 0;
    pthread_mutex_lock(&fridge_lock);
    if (!fridge_db) {
        pthread_mutex_unlock(&fridge_lock);
        return -1;
    }
    st = NULL;
    rc = sqlite3_prepare_v2(fridge_db,
        "SELECT name FROM r2_fridge_items WHERE quantity>0 AND fullness>0 LIMIT 1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int step = sqlite3_step(st);
        if (step == SQLITE_ROW) available = 1;
        else if (step != SQLITE_DONE) rc = step;
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_OK ? available : -1;
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
    snprintf(summary,sizeof(summary),"R2's modeled enjoyment rating for %.200s was %+d/2.",food,satisfaction);
    snprintf(details,sizeof(details),"Food=%.160s; ingredients=%.400s; sensory description=%.400s; enjoyment_rating=%d/2; reason=%.400s. This is a learned subjective simulation, not an externally verified reaction.",
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
    if (!r2_reality_is_initialized() || !isfinite(hours) ||
        hours <= 0.0 || hours > 48.0) return -1;
    if (r2_reality_tick() != 0) return -1;
    pthread_mutex_lock(&reality_lock);
    int rc = sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK && update_hunger_locked(hours * 3600.0) != 0)
        rc = SQLITE_ERROR;
    sqlite3_stmt *st = NULL;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(reality_db,
            "UPDATE r2_reality_self SET sleepiness=MAX(0,sleepiness-?), energy=MIN(100,energy+?), updated_at=CURRENT_TIMESTAMP WHERE id=1",
            -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_double(st, 1, hours * 12.5);
        sqlite3_bind_double(st, 2, hours * 10.0);
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(reality_db) != 1) rc = SQLITE_NOTFOUND;
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc == SQLITE_OK) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
    pthread_mutex_unlock(&reality_lock);
    if (rc != SQLITE_OK) return -1;
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
    sqlite3_int64 cash=0,bank=0;
    pthread_mutex_lock(&reality_lock);
    int ok=money_read_locked(&cash,&bank)==0;
    pthread_mutex_unlock(&reality_lock);
    if (!ok) return NULL;
    money_mirror_write(cash,bank);
    char out[768];
    snprintf(out,sizeof(out),
        "MONEY ACCOUNT (derived from physical files): carried cash=$%.2f; piggybank=$%.2f; total=$%.2f. Each money/money(N) file is $1; change.txt stores cents. Deleting a bill removes that dollar permanently; no restart refill occurs.",
        cash/100.0,bank/100.0,(cash+bank)/100.0);
    return strdup(out);
}
static int money_transfer(double amount, int deposit)
{
    if (!r2_reality_is_initialized() || !isfinite(amount) ||
        amount <= 0.0 || amount > 1000000.0) return -1;
    sqlite3_int64 cents = (sqlite3_int64)llround(amount * 100.0);
    if (cents <= 0 || fabs(amount * 100.0 - (double)cents) > 0.0001) return -1;

    char wallet[1200], piggybank[1200];
    money_dir_path(0, wallet, sizeof(wallet));
    money_dir_path(1, piggybank, sizeof(piggybank));
    pthread_mutex_lock(&purchase_lock);
    pthread_mutex_lock(&reality_lock);

    sqlite3_int64 cash = 0, bank = 0;
    sqlite3_int64 old_cash = 0, old_bank = 0;
    int rc = purchase_journal_recover();
    if (rc == 0) rc = money_journal_recover(NULL);
    if (rc == 0) rc = money_read_locked(&cash, &bank);
    old_cash = cash;
    old_bank = bank;
    if (rc == 0 && ((deposit && cash < cents) || (!deposit && bank < cents)))
        rc = -1;

    sqlite3_int64 new_cash = cash, new_bank = bank;
    if (rc == 0) {
        new_cash = deposit ? cash - cents : cash + cents;
        new_bank = deposit ? bank + cents : bank - cents;
        if (new_cash < 0 || new_bank < 0 ||
            new_cash > 100000000LL || new_bank > 100000000LL) rc = -1;
    }

    int journal_started = 0;
    if (rc == 0) {
        rc = money_journal_begin(old_cash, old_bank, new_cash, new_bank,
                                 deposit ? "deposit" : "withdrawal");
        journal_started = rc == 0;
    }
    if (rc == 0 && money_set_dir_balance(wallet, new_cash, NULL) != 0) rc = -1;
#ifdef R2_MONEY_TEST_FAILURE_INJECTION
    if (rc == 0 && getenv("R2_TEST_MONEY_TRANSFER_FAIL_AFTER_WALLET")) rc = -1;
#endif
    if (rc == 0 && money_set_dir_balance(piggybank, new_bank, NULL) != 0) rc = -1;
    if (rc == 0 && money_read_locked(&cash, &bank) != 0) rc = -1;

    if (rc == 0) {
        if (money_journal_mark_committed() == 0) {
            /* A committed journal is safe to leave behind; startup can clean it. */
            (void)money_journal_clear();
        } else {
            int was_committed = 0;
            if (money_journal_recover(&was_committed) != 0 || !was_committed) rc = -1;
            else rc = 0; /* The durable commit marker won despite an ambiguous return. */
        }
    } else if (journal_started) {
        int was_committed = 0;
        if (money_journal_recover(&was_committed) != 0) {
            fprintf(stderr, "[R2 Money] CRITICAL: transfer rollback is pending; durable journal retained for startup recovery.\n");
        } else if (was_committed) {
            rc = 0;
        }
    }

    if (money_read_locked(&cash, &bank) != 0) {
        if (rc == 0) rc = -1;
        fprintf(stderr, "[R2 Money] Could not refresh derived account summary after transfer.\n");
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc == 0 || journal_started) money_mirror_write(cash, bank);
    pthread_mutex_unlock(&purchase_lock);
    return rc == 0 ? 0 : -1;
}

int r2_reality_money_receive(double amount)
{
    if (!r2_reality_is_initialized() || !isfinite(amount) ||
        amount <= 0.0 || amount > 1000000.0) return -1;
    sqlite3_int64 cents = (sqlite3_int64)llround(amount * 100.0);
    if (cents <= 0 || fabs(amount * 100.0 - (double)cents) > 0.0001) return -1;

    char wallet[1200];
    money_dir_path(0, wallet, sizeof(wallet));
    pthread_mutex_lock(&purchase_lock);
    pthread_mutex_lock(&reality_lock);
    sqlite3_int64 cash = 0, bank = 0;
    int rc = purchase_journal_recover();
    if (rc == 0) rc = money_read_locked(&cash, &bank);
    sqlite3_int64 old_cash = cash;
    int wallet_changed = 0;
    if (rc == 0 && cash <= 100000000LL - cents) {
        if (money_set_dir_balance(wallet, cash + cents, &wallet_changed) != 0 ||
            money_read_locked(&cash, &bank) != 0) rc = -1;
    } else {
        rc = -1;
    }
    if (rc != 0 && wallet_changed) {
        int wallet_restore = money_set_dir_balance(wallet, old_cash, NULL);
        int mirror_restore = money_read_locked(&cash, &bank);
        if (wallet_restore != 0 || mirror_restore != 0)
            fprintf(stderr, "[R2 Money] CRITICAL: cash receipt failed and wallet rollback was incomplete; inspect Wallet files.\n");
    }
    pthread_mutex_unlock(&reality_lock);
    if (rc == 0 || wallet_changed) money_mirror_write(cash, bank);
    pthread_mutex_unlock(&purchase_lock);
    return rc == 0 ? 0 : -1;
}
int r2_reality_money_deposit(double amount){return money_transfer(amount,1);}
int r2_reality_money_withdraw(double amount){return money_transfer(amount,0);}
static int purchase_inventory_state(const char *name, const char *destination,
                                   sqlite3_int64 *quantity, char *container, size_t cap)
{
    if (quantity) *quantity = 0;
    if (container && cap) container[0] = '\0';
    if (!name || !destination) return -1;
    sqlite3 *db = !strcasecmp(destination, "fridge") ? fridge_db : reality_db;
    if (!db) return -1;
    /* Callers hold reality_lock while inspecting the purchase journal.
       Fridge has its own database/lock; the Reality DB must not be relocked. */
    int is_fridge = !strcasecmp(destination, "fridge");
    if (is_fridge) pthread_mutex_lock(&fridge_lock);
    sqlite3_stmt *st = NULL;
    int rc;
    if (!strcasecmp(destination, "fridge")) {
        rc = sqlite3_prepare_v2(db,
            "SELECT quantity FROM r2_fridge_items WHERE name=? COLLATE NOCASE",
            -1, &st, NULL);
    } else {
        rc = sqlite3_prepare_v2(db,
            "SELECT quantity,container FROM r2_reality_objects WHERE name=? COLLATE NOCASE",
            -1, &st, NULL);
    }
    if (rc == SQLITE_OK) {
        bind_text(st, 1, name);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (quantity) *quantity = sqlite3_column_int64(st, 0);
            if (container && cap) {
                const unsigned char *c = !strcasecmp(destination, "fridge")
                    ? (const unsigned char *)"fridge" : sqlite3_column_text(st, 1);
                snprintf(container, cap, "%s", c ? (const char *)c : "");
            }
            rc = SQLITE_DONE;
        } else if (rc == SQLITE_DONE) {
            if (quantity) *quantity = 0;
            if (container && cap) container[0] = '\0';
        }
    }
    if (st) sqlite3_finalize(st);
    if (is_fridge) pthread_mutex_unlock(&fridge_lock);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int purchase_journal_begin(sqlite3_int64 old_cash, sqlite3_int64 old_bank,
                                  sqlite3_int64 new_cash, sqlite3_int64 new_bank,
                                  const char *name, const char *description,
                                  const char *destination, sqlite3_int64 pre_quantity,
                                  const char *pre_container)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_prepare_v2(reality_db,
        "INSERT INTO r2_purchase_journal(id,old_cash_cents,old_bank_cents,new_cash_cents,new_bank_cents,item_name,item_description,destination,pre_quantity,pre_container,phase) "
        "VALUES(1,?,?,?,?,?,?,?,?,?,'pending')",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, old_cash);
        sqlite3_bind_int64(st, 2, old_bank);
        sqlite3_bind_int64(st, 3, new_cash);
        sqlite3_bind_int64(st, 4, new_bank);
        bind_text(st, 5, name);
        bind_text(st, 6, description);
        bind_text(st, 7, destination);
        sqlite3_bind_int64(st, 8, pre_quantity);
        bind_text(st, 9, pre_container);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int purchase_journal_set_phase(const char *phase)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_exec(reality_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_prepare_v2(reality_db,
        "UPDATE r2_purchase_journal SET phase=? WHERE id=1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        bind_text(st, 1, phase);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(reality_db) == 1)
        rc = sqlite3_exec(reality_db, "COMMIT", NULL, NULL, NULL);
    else rc = SQLITE_ERROR;
    if (rc != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

static int purchase_journal_clear(void)
{
    if (sqlite3_exec(reality_db,
        "BEGIN IMMEDIATE; DELETE FROM r2_purchase_journal WHERE id=1; COMMIT;",
        NULL, NULL, NULL) != SQLITE_OK) {
        (void)sqlite3_exec(reality_db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

/* A pending purchase is committed only when the intended inventory stack
   visibly advanced in the intended container. Otherwise recovery refunds it. */
static int purchase_journal_recover(void)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 old_cash=0, old_bank=0, new_cash=0, new_bank=0, pre_quantity=0;
    char name[REALITY_MAX_TEXT+1]={0}, destination[REALITY_MAX_TEXT+1]={0};
    char pre_container[REALITY_MAX_TEXT+1]={0}, phase[24]={0};
    int rc = sqlite3_prepare_v2(reality_db,
        "SELECT old_cash_cents,old_bank_cents,new_cash_cents,new_bank_cents,item_name,destination,pre_quantity,pre_container,phase "
        "FROM r2_purchase_journal WHERE id=1", -1, &st, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { sqlite3_finalize(st); return 0; }
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    old_cash=sqlite3_column_int64(st,0); old_bank=sqlite3_column_int64(st,1);
    new_cash=sqlite3_column_int64(st,2); new_bank=sqlite3_column_int64(st,3);
    snprintf(name,sizeof(name),"%s",sqlite3_column_text(st,4) ? (const char *)sqlite3_column_text(st,4) : "");
    snprintf(destination,sizeof(destination),"%s",sqlite3_column_text(st,5) ? (const char *)sqlite3_column_text(st,5) : "");
    pre_quantity=sqlite3_column_int64(st,6);
    snprintf(pre_container,sizeof(pre_container),"%s",sqlite3_column_text(st,7) ? (const char *)sqlite3_column_text(st,7) : "");
    snprintf(phase,sizeof(phase),"%s",sqlite3_column_text(st,8) ? (const char *)sqlite3_column_text(st,8) : "");
    sqlite3_finalize(st);

    sqlite3_int64 current_quantity=0;
    char current_container[REALITY_MAX_TEXT+1]={0};
    if (purchase_inventory_state(name,destination,&current_quantity,current_container,sizeof(current_container)) != 0)
        return -1;
    int delivered = current_quantity > pre_quantity &&
        !strcasecmp(current_container,destination);
    int keep_charge = delivered || !strcmp(phase,"committed");
    if (!keep_charge && strcmp(phase,"pending")) return -1;

    char wallet[1200], piggybank[1200];
    money_dir_path(0,wallet,sizeof(wallet));
    money_dir_path(1,piggybank,sizeof(piggybank));
    if (money_set_dir_balance(wallet,keep_charge ? new_cash : old_cash,NULL) != 0 ||
        money_set_dir_balance(piggybank,keep_charge ? new_bank : old_bank,NULL) != 0 ||
        money_read_locked(NULL,NULL) != 0) {
        fprintf(stderr,"[R2 Money] Purchase recovery could not reconcile physical balances; journal retained.\\n");
        return -1;
    }
    if (purchase_journal_clear() != 0) return -1;
    money_mirror_write(keep_charge ? new_cash : old_cash,keep_charge ? new_bank : old_bank);
    return 0;
}

int r2_reality_buy_item(const char *name, const char *description,
                       double price, const char *container)
{
    if (!name || !*name || strlen(name)>REALITY_MAX_TEXT ||
        (description && strlen(description)>REALITY_MAX_TEXT) ||
        !isfinite(price) || price <= 0.0 || price > 1000000.0 ||
        !r2_reality_is_initialized()) return -1;
    sqlite3_int64 cents = (sqlite3_int64)llround(price * 100.0);
    if (cents <= 0 || fabs(price * 100.0 - (double)cents) > 0.0001) return -1;

    char destination_buf[REALITY_MAX_TEXT+1];
    canonical_container_name(container && *container ? container : "pockets",
                             destination_buf,sizeof(destination_buf));
    const char *destination = destination_buf;
    if (!container_accessible(destination)) return -1;
    if (!strcasecmp(destination,"fridge")) {
        double fullness=0.0, energy=0.0;
        if (!reality_is_home() || food_metric(name,&fullness,&energy)!=0) return -1;
    }

    char wallet[1200], piggybank[1200];
    money_dir_path(0,wallet,sizeof(wallet));
    money_dir_path(1,piggybank,sizeof(piggybank));
    pthread_mutex_lock(&purchase_lock);
    pthread_mutex_lock(&reality_lock);
    if (inventory_transfer_recover_locked()!=0 ||
        purchase_journal_recover()!=0 || money_journal_recover(NULL)!=0) {
        pthread_mutex_unlock(&reality_lock);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
    sqlite3_int64 pre_quantity=0;
    char pre_container[REALITY_MAX_TEXT+1]={0};
    if (purchase_inventory_state(name,destination,&pre_quantity,pre_container,sizeof(pre_container))!=0) {
        pthread_mutex_unlock(&reality_lock);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
    sqlite3_int64 cash=0, bank=0;
    int rc=money_read_locked(&cash,&bank);
    sqlite3_int64 old_cash=cash, old_bank=bank;
    if (rc==0 && cash+bank<cents) rc=-1;
    sqlite3_int64 new_cash=cash, new_bank=bank;
    if (rc==0) {
        sqlite3_int64 spend_cash=cash<cents?cash:cents;
        new_cash=cash-spend_cash;
        new_bank=bank-(cents-spend_cash);
        if (new_cash<0 || new_bank<0) rc=-1;
    }
    if (rc==0)
        rc=purchase_journal_begin(old_cash,old_bank,new_cash,new_bank,name,
            description ? description : "Purchased item",destination,pre_quantity,pre_container);
    if (rc==0 && (money_set_dir_balance(wallet,new_cash,NULL)!=0 ||
                  money_set_dir_balance(piggybank,new_bank,NULL)!=0 ||
                  money_read_locked(&cash,&bank)!=0)) rc=-1;
    if (rc!=0) {
        /* If an intent was persisted, recovery owns rollback; never erase it
           unless restoring the original balances succeeded. */
        if (purchase_journal_recover()!=0)
            fprintf(stderr,"[R2 Money] Purchase failed and recovery remains pending; journal retained.\\n");
        pthread_mutex_unlock(&reality_lock);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
#ifdef R2_MONEY_TEST_FAILURE_INJECTION
    if (getenv("R2_TEST_PURCHASE_INTERRUPT_BEFORE_ITEM")) {
        /* Leave durable intent and deducted money exactly as a crash would. */
        pthread_mutex_unlock(&reality_lock);
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
#endif
    pthread_mutex_unlock(&reality_lock);

    if (r2_reality_add_item(name,description ? description : "Purchased item",destination,1)!=0) {
        pthread_mutex_lock(&reality_lock);
        int recovery_rc=purchase_journal_recover();
        pthread_mutex_unlock(&reality_lock);
        if (recovery_rc!=0)
            fprintf(stderr,"[R2 Money] Item creation failed; purchase recovery remains pending.\\n");
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
#ifdef R2_MONEY_TEST_FAILURE_INJECTION
    if (getenv("R2_TEST_PURCHASE_INTERRUPT_AFTER_ITEM")) { pthread_mutex_unlock(&purchase_lock); return -1; }
#endif
    pthread_mutex_lock(&reality_lock);
    rc=purchase_journal_set_phase("committed");
    /* Keep the committed intent until physical balances have been re-read;
       if SQLite summary refresh fails, startup can still finish reconciliation. */
    if (rc==0 && money_read_locked(&cash,&bank)!=0) rc=-1;
    if (rc==0) rc=purchase_journal_clear();
    pthread_mutex_unlock(&reality_lock);
    if (rc!=0) {
        /* The item exists. Startup recovery can infer delivery and finish charging. */
        fprintf(stderr,"[R2 Money] Purchase delivered but journal finalization failed; recovery will reconcile it.\\n");
        pthread_mutex_unlock(&purchase_lock);
        return -1;
    }
    money_mirror_write(cash,bank);
    pthread_mutex_unlock(&purchase_lock);
    char summary[512], details[1024];
    snprintf(summary,sizeof(summary),"R2 purchased %.350s for $%.2f.",name,cents/100.0);
    snprintf(details,sizeof(details),"Item=%.400s; price=$%.2f; destination=%.200s; wallet cash paid first, then piggybank.",name,cents/100.0,destination);
    bridge_event("purchase",summary,details,1,1);
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

int r2_reality_tv_display_event(int opened)
{
    if (!reality_db || !reality_ready || (opened != 0 && opened != 1)) return -1;
    if (opened) {
        bridge_event("tv_display_opened",
                     "R2's CRT television window opened, directing his modeled visual attention to the display.",
                     "The TV interface was launched in R2's environment. This records the forced attention cue requested for the display; it does not claim that Eyes captured or interpreted video frames.",
                     1, 0);
    } else {
        bridge_event("tv_display_closed",
                     "R2's CRT television window closed.",
                     "The TV interface closed; this does not imply that the modeled television was powered off or that a tape was ejected.",
                     0, 0);
    }
    return 0;
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
