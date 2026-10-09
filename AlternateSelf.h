#ifndef R2_ALTERNATE_SELF_H
#define R2_ALTERNATE_SELF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int r2_altself_init(void);
void r2_altself_shutdown(void);

/* Each branch is explicitly hypothetical and never saved as factual memory. */
int64_t r2_altself_create(const char *name, const char *scenario,
                         const char *assumptions, const char *predicted_outcome,
                         const char *conclusion, int64_t evidence_event_id);
int r2_altself_discard(int64_t branch_id);
int r2_altself_retain_hypothesis(int64_t branch_id);
char *r2_altself_list(int limit);
char *r2_altself_show(int64_t branch_id);
char *r2_altself_compare(int64_t first_id, int64_t second_id);

#ifdef __cplusplus
}
#endif
#endif
