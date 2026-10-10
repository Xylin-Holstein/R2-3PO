#ifndef R2_IMAGINATION_H
#define R2_IMAGINATION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Imagination is a capability over R2's existing systems, not a parallel
 * memory database. Imagined scenarios are retained through the Choice Lab. */
int r2_imagination_is_initialized(void);
int r2_imagination_init(void);
void r2_imagination_shutdown(void);

/* Caller frees returned text. A successful scenario is saved as a hypothetical
 * Choice Lab branch and is never written as a factual observation. */
char *r2_imagination_create(const char *request);

/* User/evidence feedback: accurate earns positive reinforcement; partial,
 * incorrect, and unresolved outcomes never apply a penalty. */
int r2_imagination_feedback(long long branch_id, const char *assessment,
                            const char *notes);

#ifdef __cplusplus
}
#endif
#endif
