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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_prompt[12000];
static int branch_creates;
static int feedback_events;
static int links;
static int reward_calls;
static int rewards_applied;
static int reward_already_applied;

static int check(int ok, const char *message)
{
    if (!ok) fprintf(stderr, "FAIL: %s\n", message);
    return ok;
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
    return strdup("REALITY_SENTINEL: current modeled room and self-state; NO_FRIDGE_SENTINEL.");
}
int r2_visual_is_initialized(void) { return 1; }
char *r2_visual_search(const char *query, int limit)
{
    (void)query; (void)limit; return strdup("VISUAL_SENTINEL: stored visual experience.");
}
char *r2_reward_context(void) { return strdup("REWARD_SENTINEL: learned feedback."); }
char *r2_addiction_report(void) { return strdup("HABIT_SENTINEL: repeated preference history."); }
char *r2_altself_list(int limit)
{
    (void)limit; return strdup("CHOICE_SENTINEL: prior hypothetical branch.");
}
char *r2_fridge_context(void) { return strdup("FRIDGE_SENTINEL: current fridge inventory."); }

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
    (void)category; (void)type; (void)summary; (void)details; (void)source;
    ++feedback_events; return feedback_events;
}
int64_t r2_altself_create(const char *name, const char *scenario,
                          const char *assumptions, const char *predicted_outcome,
                          const char *conclusion, int64_t evidence_event_id)
{
    (void)name; (void)scenario; (void)assumptions; (void)predicted_outcome;
    (void)conclusion; (void)evidence_event_id; ++branch_creates;
    return 40 + branch_creates;
}
char *r2_altself_show(int64_t id)
{
    if (id == 41 || id == 42) return strdup("id|Imagination: scenario|active");
    return strdup("No Alternate-Self branches found.");
}
int r2_altself_link_event(int64_t branch_id, int64_t event_id,
                          const char *relationship, const char *notes)
{
    (void)branch_id; (void)event_id; (void)relationship; (void)notes;
    ++links; return 0;
}
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
    return strdup("Hypothetical only: the remembered CRT room frames the imagined movie scene.");
}

int main(void)
{
    int passed = 0;
    char *result = NULL;

    if (!check(r2_imagination_init() == 0, "initialize imagination")) goto done;
    result = r2_imagination_create("Imagine the first movie in the CRT room.");
    if (!check(result && strstr(result, "hypothetical imagination"),
               "generate and persist a hypothetical scenario")) goto done;
    if (!check(strstr(last_prompt, "MEMORY_SENTINEL")
               && strstr(last_prompt, "CONVERSATION_SENTINEL")
               && strstr(last_prompt, "DIARY_SENTINEL")
               && strstr(last_prompt, "LIFELOG_SENTINEL")
               && strstr(last_prompt, "REALITY_SENTINEL")
               && strstr(last_prompt, "VISUAL_SENTINEL")
               && strstr(last_prompt, "REWARD_SENTINEL")
               && strstr(last_prompt, "HABIT_SENTINEL")
               && strstr(last_prompt, "CHOICE_SENTINEL"),
               "the generated scenario receives all relevant existing context sources")) goto done;
    if (!check(!strstr(last_prompt, "FRIDGE_SENTINEL")
               && !strstr(last_prompt, "NO_FRIDGE_SENTINEL"),
               "general imagination excludes fridge stock")) goto done;
    free(result); result = NULL;

    result = r2_imagination_create("Imagine what I can make with what I have in the fridge.");
    if (!check(result && strstr(last_prompt, "FRIDGE_SENTINEL"),
               "current fridge inventory is retrieved for explicit inventory imagination")) goto done;
    free(result); result = NULL;

    if (!check(r2_imagination_feedback(41, "incorrect", "The later observation contradicted it.") == 0,
               "incorrect imagination can be recorded without punishment")) goto done;
    if (!check(rewards_applied == 0, "incorrect imagination applies no reward or penalty")) goto done;
    if (!check(r2_imagination_feedback(41, "accurate", "") == -2,
               "positive feedback requires evidence notes")) goto done;
    if (!check(rewards_applied == 0, "missing evidence does not reward")) goto done;
    if (!check(r2_imagination_feedback(41, "accurate", "Later direct observation matched the prediction.") == 0,
               "evidence-backed accurate feedback is accepted")) goto done;
    if (!check(rewards_applied == 1 && links == 2,
               "feedback is linked to the hypothetical branch and accurate feedback rewards once")) goto done;
    if (!check(r2_imagination_feedback(41, "accurate", "The same observation still matches.") == 2
               && reward_calls == 2 && rewards_applied == 1,
               "a repeated accurate result cannot apply a second reward")) goto done;

    passed = 1;
done:
    free(result);
    r2_imagination_shutdown();
    if (passed) puts("Imagination runtime contract smoke tests passed.");
    return passed ? 0 : 1;
}
