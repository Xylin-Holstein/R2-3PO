#ifndef R2_REWARD_H
#define R2_REWARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small, persistent, behavior-linked reinforcement. Positive outcomes are
 * limited to +1..+5; penalties are limited to -1..-7. Enjoyment modifiers
 * are temporary and decay rather than permanently rewriting preferences. */
int r2_reward_init(void);
/* Retry cross-linking persisted reward rows to Life Log events after startup. */
int r2_reward_reconnect_history(int limit);
void r2_reward_shutdown(void);
int r2_reward_apply(const char *target, const char *source, int points,
                    const char *reason, int voluntary_choice);
/* Apply once for a persistent target/source pair: 0=applied, 1=already applied, -1=error. */
int r2_reward_apply_once(const char *target, const char *source, int points,
                          const char *reason, int voluntary_choice);
int r2_reward_current_modifier(const char *target);
char *r2_reward_context(void); /* Caller frees. */

/* Evaluate the explicitly configured diary correction rule without sending
 * private diary prose to the public Life Log or Observer. */
int r2_reward_review_diary(int64_t diary_entry_id, const char *entry);

#ifdef __cplusplus
}
#endif
#endif
