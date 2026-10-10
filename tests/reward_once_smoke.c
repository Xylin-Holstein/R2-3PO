#define _POSIX_C_SOURCE 200809L
#include "Reward.h"
#include "Log.h"
#include "r2.h"
#include "Addiction.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define THREADS 24
static pthread_barrier_t start_barrier;
static int results[THREADS];

int r2_log_is_initialized(void) { return 0; }
int64_t r2_log_event_with_memory(R2LogCategory category, const char *event_type,
                                 const char *summary, const char *details,
                                 const char *source, int save_as_memory)
{
    (void)category; (void)event_type; (void)summary; (void)details;
    (void)source; (void)save_as_memory; return -1;
}
int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes)
{
    (void)from_event_id; (void)to_event_id; (void)relationship; (void)notes;
    return 0;
}
int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category; return -1;
}
int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details; return -1;
}

static void *worker(void *arg)
{
    intptr_t i = (intptr_t)arg;
    (void)pthread_barrier_wait(&start_barrier);
    results[i] = r2_reward_apply_once(
        "imagination_branch_concurrency_test", "verified_imagination", 1,
        "Concurrent duplicate protection test", 0);
    return NULL;
}

static int count_rewards(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int count = -1;
    if (sqlite3_open(R2_HOME "/r2_rewards.db", &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM reward_events WHERE target=? AND source=?",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, "imagination_branch_concurrency_test", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, "verified_imagination", -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return count;
}

int main(void)
{
    const char *root = R2_HOME;
    (void)mkdir(R2_ROOT, 0700);
    (void)mkdir(root, 0700);
    char path[1024];
    snprintf(path, sizeof(path), "%s/r2_rewards.db", root); unlink(path);
    snprintf(path, sizeof(path), "%s/r2_rewards.db-wal", root); unlink(path);
    snprintf(path, sizeof(path), "%s/r2_rewards.db-shm", root); unlink(path);

    pthread_t threads[THREADS];
    int applied = 0, already = 0, failed = 0;
    if (r2_reward_init() != 0) return 1;
    if (pthread_barrier_init(&start_barrier, NULL, THREADS) != 0) return 1;

    for (intptr_t i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) return 1;
    }
    for (int i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&start_barrier);

    for (int i = 0; i < THREADS; ++i) {
        if (results[i] == 0) ++applied;
        else if (results[i] == 1) ++already;
        else ++failed;
    }
    if (applied != 1 || already != THREADS - 1 || failed != 0 ||
        count_rewards() != 1) {
        fprintf(stderr, "FAIL: atomic once reward (applied=%d already=%d failed=%d rows=%d)\n",
                applied, already, failed, count_rewards());
        r2_reward_shutdown();
        return 1;
    }

    r2_reward_shutdown();
    if (r2_reward_init() != 0) return 1;
    int persisted = r2_reward_apply_once(
        "imagination_branch_concurrency_test", "verified_imagination", 1,
        "Must remain idempotent after restart", 0);
    if (persisted != 1 || count_rewards() != 1) {
        fprintf(stderr, "FAIL: one-time reward key persists across restart\n");
        r2_reward_shutdown();
        return 1;
    }
    r2_reward_shutdown();
    puts("Atomic one-time reward concurrency and restart tests passed.");
    return 0;
}
