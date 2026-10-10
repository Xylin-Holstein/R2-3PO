#ifndef R2_ADDICTION_H
#define R2_ADDICTION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent, behavior-derived preference/habit evaluator.
 * Enjoyment is 0..100. A high score alone never establishes addiction:
 * labels also require repeated voluntary choices spread across multiple days.
 */
int r2_addiction_init(void);
void r2_addiction_shutdown(void);
int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details);
int r2_addiction_rate_enjoyment(const char *target, const char *target_type,
                                int enjoyment_0_100, const char *source,
                                const char *details);
/* Caller frees the returned report. */
char *r2_addiction_report(void);

#ifdef __cplusplus
}
#endif
#endif
