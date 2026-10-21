#ifndef R2_IMAGINATION_H
#define R2_IMAGINATION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize and shut down the hypothetical-experience subsystem. */
int r2_imagination_init(void);
void r2_imagination_shutdown(void);

/*
 * Generate a context-informed hypothetical scenario and store it as a
 * clearly labeled Choice Lab branch. The returned string is heap allocated.
 */
char *r2_imagination_create(const char *request);

/*
 * Record explicit evidence-based feedback for a saved imagination branch.
 * Accepted assessments: accurate, partial, incorrect, unresolved.
 * Inaccurate/unresolved outcomes are not punished.
 */
int r2_imagination_feedback(long long branch_id, const char *assessment,
                            const char *notes);

#ifdef __cplusplus
}
#endif
#endif
