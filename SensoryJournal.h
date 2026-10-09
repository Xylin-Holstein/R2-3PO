#ifndef R2_SENSORY_JOURNAL_H
#define R2_SENSORY_JOURNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A searchable sensory/thinking index over original Life Log events. */
int r2_sj_init(void);
void r2_sj_shutdown(void);

/* Called by the Life Log after a real event has been committed. */
void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source);

/* Record a journal note as a real thinking/journal event. */
int64_t r2_sj_note(const char *note, const char *details);

/* Returned text is allocated; caller must free(). */
char *r2_sj_recent(int limit);
char *r2_sj_search(const char *query, int limit);
/* Returns indexed entry count, or -1 if the journal is unavailable. */
long r2_sj_count(void);

#ifdef __cplusplus
}
#endif
#endif
