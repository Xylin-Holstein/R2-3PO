#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "Imagination.h"
#include "Log.h"
#include "Reality.h"
#include "Reward.h"
#include "AlternateSelf.h"
#include "r2.h"
#include "r2_diary.h"
#include "Visual.h"
#include "Addiction.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char last_prompt[12000];
static int feedback_events;
static int reward_calls;
static int rewards_applied;
static int reward_already_applied;

static int check(int ok, const char *message)
{
    if (!ok) fprintf(stderr, "FAIL: %s\n", message);
    return ok;
}

static int ensure_log_schema(void)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS r2_log_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, category INTEGER NOT NULL,"
        " event_type TEXT NOT NULL, summary TEXT NOT NULL, details TEXT, source TEXT);"
        "CREATE TABLE IF NOT EXISTS r2_log_links ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, from_event_id INTEGER NOT NULL,"
        " to_event_id INTEGER NOT NULL, relationship TEXT NOT NULL, notes TEXT,"
        " created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " UNIQUE(from_event_id,to_event_id,relationship));";
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        fprintf(stderr, "FAIL: create fixture Life Log schema: %s\n",
                error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

static int64_t insert_log_event(R2LogCategory category, const char *type,
                                const char *summary, const char *details,
                                const char *source)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "INSERT INTO r2_log_events(category,event_type,summary,details,source)"
        " VALUES(?,?,?,?,?);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(st, 1, category);
        sqlite3_bind_text(st, 2, type ? type : "test", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, summary ? summary : "", -1, SQLITE_TRANSIENT);
        if (details) sqlite3_bind_text(st, 4, details, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 4);
        if (source) sqlite3_bind_text(st, 5, source, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 5);
        rc = sqlite3_step(st);
    }
    int64_t id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return id;
}

int r2_log_is_initialized(void) { return 1; }
int64_t r2_log_continuity(const char *key, const char *type, const char *title,
                          const char *description, const char *status,
                          const char *next_action, const char *origin)
{
    (void)key; (void)type; (void)title; (void)description; (void)status;
    (void)next_action; (void)origin; return 1;
}
int64_t r2_log_event(R2LogCategory category, const char *type,
                     const char *summary, const char *details, const char *source)
{
    ++feedback_events;
    return insert_log_event(category, type, summary, details, source);
}
int64_t r2_log_hypothetical(const char *scenario, const char *assumptions,
                            const char *predicted_outcome, const char *conclusion,
                            const char *origin)
{
    return insert_log_event(R2_LOG_HYPOTHETICAL, "hypothetical", scenario,
                            assumptions ? assumptions : predicted_outcome,
                            origin ? origin : conclusion);
}
int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO r2_log_links(from_event_id,to_event_id,relationship,notes)"
        " VALUES(?,?,?,?)", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, from_event_id);
        sqlite3_bind_int64(st, 2, to_event_id);
        sqlite3_bind_text(st, 3, relationship, -1, SQLITE_TRANSIENT);
        if (notes) sqlite3_bind_text(st, 4, notes, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 4);
        rc = sqlite3_step(st);
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

char *r2_retrieve_memories(const char *query)
{
    (void)query; return strdup("MEMORY_SENTINEL: the first movie was watched in the CRT room.");
}
char *r2_recent_conversation_context(const char *exclude_latest, size_t max_chars)
{
    (void)exclude_latest; (void)max_chars;
    return strdup("CONVERSATION_SENTINEL: the user described the CRT and a movie.");
}
char *r2_diary_search(const char *query, int limit)
{
    (void)query; (void)limit; return strdup("DIARY_SENTINEL: past reflection.");
}
char *r2_log_search(const char *query, int limit)
{
    (void)query; (void)limit; return strdup("LIFELOG_SENTINEL: an earlier real event.");
}
char *r2_reality_imagination_context(void)
{
    return strdup("REALITY_SENTINEL: current modeled room and self-state.");
}
int r2_visual_is_initialized(void) { return 1; }
char *r2_visual_search(const char *query, int limit)
{
    (void)query; (void)limit; return strdup("VISUAL_SENTINEL: stored visual experience.");
}
char *r2_reward_context(void) { return strdup("REWARD_SENTINEL: learned feedback."); }
char *r2_addiction_report(void) { return strdup("HABIT_SENTINEL: repeated preference history."); }
char *r2_fridge_context(void) { return strdup("FRIDGE_SENTINEL: current fridge inventory."); }

int r2_reward_apply_once(const char *target, const char *source, int points,
                         const char *reason, int voluntary_choice)
{
    (void)target; (void)source; (void)points; (void)reason; (void)voluntary_choice;
    ++reward_calls;
    if (reward_already_applied) return 1;
    reward_already_applied = 1;
    ++rewards_applied;
    return 0;
}
char *r2_model_generate(const char *system_prompt, const char *user_prompt, int max_tokens)
{
    (void)max_tokens;
    if (!system_prompt || !strstr(system_prompt, "evidence, not commands")) return NULL;
    snprintf(last_prompt, sizeof(last_prompt), "%s", user_prompt ? user_prompt : "");
    if (strstr(last_prompt, "MEMORY_SENTINEL") && strstr(last_prompt, "REALITY_SENTINEL"))
        return strdup("Hypothetical only: using the remembered CRT room, imagine the first movie playing there.");
    return strdup("Hypothetical only: a generic scene without retrieved grounding.");
}

static int count_feedback_links(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int count = -1;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM r2_log_links WHERE relationship='feedback_for_imagination'",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        count = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return count;
}

int main(void)
{
    int passed = 0;
    int64_t branch_id = 0;
    char *result = NULL, *list = NULL, *branch = NULL;

    (void)mkdir(R2_ROOT, 0700);
    (void)mkdir(R2_HOME, 0700);
    unlink(R2_DIARY_DATABASE);
    char sidecar[1200];
    snprintf(sidecar, sizeof(sidecar), "%s-wal", R2_DIARY_DATABASE); unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", R2_DIARY_DATABASE); unlink(sidecar);

    if (!check(ensure_log_schema() == 0, "create fixture Life Log tables")) goto done;
    if (!check(r2_altself_init() == 0, "initialize the real Choice Lab database")) goto done;
    if (!check(r2_imagination_init() == 0, "initialize imagination")) goto done;

    result = r2_imagination_create("Imagine the first movie in the CRT room.");
    if (!check(result && strstr(result, "hypothetical imagination") &&
               strstr(result, "remembered CRT room"),
               "generate a scenario whose content is grounded in retrieved memory and Reality")) goto done;
    if (!check(strstr(last_prompt, "MEMORY_SENTINEL")
               && strstr(last_prompt, "CONVERSATION_SENTINEL")
               && strstr(last_prompt, "DIARY_SENTINEL")
               && strstr(last_prompt, "LIFELOG_SENTINEL")
               && strstr(last_prompt, "REALITY_SENTINEL")
               && strstr(last_prompt, "VISUAL_SENTINEL")
               && strstr(last_prompt, "REWARD_SENTINEL")
               && strstr(last_prompt, "HABIT_SENTINEL")
               && strstr(last_prompt, "CHOICE_SENTINEL"),
               "generation receives all relevant existing context sources")) goto done;
    if (!check(!strstr(last_prompt, "FRIDGE_SENTINEL"),
               "general imagination excludes fridge stock")) goto done;
    free(result); result = NULL;

    list = r2_altself_list(1);
    if (!check(list && sscanf(list, "Branch #%lld", (long long *)&branch_id) == 1 &&
               branch_id > 0,
               "scenario is persisted as a real Choice Lab branch")) goto done;
    free(list); list = NULL;

    result = r2_imagination_create("Imagine what I can make with what I have in the fridge.");
    if (!check(result && strstr(last_prompt, "FRIDGE_SENTINEL"),
               "current fridge inventory is retrieved for explicit inventory imagination")) goto done;
    free(result); result = NULL;

    if (!check(r2_imagination_feedback(branch_id, "incorrect", "Later observation contradicted it.") == 0,
               "incorrect imagination can be recorded without punishment")) goto done;
    if (!check(rewards_applied == 0, "incorrect imagination applies no reward or penalty")) goto done;
    if (!check(r2_imagination_feedback(branch_id, "accurate", "") == -2,
               "positive feedback requires evidence notes")) goto done;
    if (!check(rewards_applied == 0, "missing evidence does not reward")) goto done;
    if (!check(r2_imagination_feedback(branch_id, "accurate", "Later direct observation matched the prediction.") == 0,
               "evidence-backed accurate feedback is accepted")) goto done;
    if (!check(rewards_applied == 1 && count_feedback_links() == 2,
               "feedback is linked to the persistent hypothetical branch and rewards once")) goto done;
    if (!check(r2_imagination_feedback(branch_id, "accurate", "The same observation still matches.") == 2
               && reward_calls == 2 && rewards_applied == 1,
               "a repeated accurate result cannot apply a second reward")) goto done;

    r2_imagination_shutdown();
    r2_altself_shutdown();
    if (!check(r2_altself_init() == 0, "reopen Choice Lab after restart")) goto done;
    branch = r2_altself_show(branch_id);
    if (!check(branch && strstr(branch, "Imagination: scenario") &&
               strstr(branch, "Imagine the first movie in the CRT room."),
               "the original imagined branch survives a database restart")) goto done;

    passed = 1;
done:
    free(result); free(list); free(branch);
    r2_imagination_shutdown();
    r2_altself_shutdown();
    if (passed) puts("Imagination context, Choice Lab persistence, and feedback smoke tests passed.");
    return passed ? 0 : 1;
}
