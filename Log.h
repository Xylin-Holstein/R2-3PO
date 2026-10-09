#ifndef R2_LOG_H
#define R2_LOG_H

/*
 * ============================================================
 * R2-3PO LIFE LOG PUBLIC INTERFACE
 * ============================================================
 *
 * Log.c records the chronological, factual history of R2's
 * operation and experiences. It is deliberately separate from
 * the reflective diary implemented by r2_diary.c.
 *
 * The Life Log records events. The diary reflects on events.
 * Persistent conversational memory can reference selected
 * Life Log entries without replacing the underlying records.
 *
 * Database: R2_DIARY_DATABASE (r2_memory.db)
 * Tables are namespaced with r2_log_ and do not alter the
 * existing memories or diary_entries tables.
 * ============================================================
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum R2LogCategory {
    R2_LOG_SYSTEM = 0,
    R2_LOG_LIFECYCLE,
    R2_LOG_SENSORY,
    R2_LOG_THINKING,
    R2_LOG_FILESYSTEM,
    R2_LOG_MEDIA,
    R2_LOG_WORLD,
    R2_LOG_MILESTONE,
    R2_LOG_MEMORY,
    R2_LOG_EXPERIMENT,
    R2_LOG_ERROR,
    R2_LOG_EMOTION,
    R2_LOG_BELIEF,
    R2_LOG_CONTINUITY,
    R2_LOG_CONVERSATION,
    R2_LOG_HYPOTHETICAL
} R2LogCategory;

/*
 * Initialize the Life Log after the shared memory database and
 * required R2 directories exist.
 * Returns 0 on success, -1 on failure.
 */
int r2_log_init(void);

/* Flush and close the Life Log database connection. */
void r2_log_shutdown(void);

/*
 * Record a factual event.
 *
 * category: one of R2LogCategory.
 * event_type: short machine-readable label, e.g. "startup".
 * summary: concise human-readable event description.
 * details: optional additional context; may be NULL.
 * source: optional subsystem/source label; may be NULL.
 *
 * Returns the inserted event ID on success, -1 on failure.
 */
int64_t r2_log_event(
    R2LogCategory category,
    const char *event_type,
    const char *summary,
    const char *details,
    const char *source
);

/*
 * Record an event and, if save_as_memory is non-zero, attempt to
 * save a concise pointer/summary through the existing R2 memory
 * interface. The log event remains authoritative if memory save
 * fails. Returns the Life Log event ID or -1.
 */
int64_t r2_log_event_with_memory(
    R2LogCategory category,
    const char *event_type,
    const char *summary,
    const char *details,
    const char *source,
    int save_as_memory
);

/* Lifecycle/session helpers. */
int r2_log_session_start(void);
int r2_log_session_end(const char *reason);

/* Record the first verified occurrence of a named milestone. */
int64_t r2_log_milestone(
    const char *milestone_key,
    const char *description,
    const char *details
);

/* Specialized event helpers. */
int64_t r2_log_sensory(
    const char *sense,
    const char *observation,
    const char *details,
    const char *source
);

int64_t r2_log_thinking(
    const char *thought_kind,
    const char *summary,
    const char *details
);

int64_t r2_log_file_event(
    const char *action,
    const char *path,
    const char *result,
    const char *details
);

int64_t r2_log_media_event(
    const char *action,
    const char *media_type,
    const char *path_or_identifier,
    const char *details
);

int64_t r2_log_world_event(
    const char *object_or_device,
    const char *event,
    const char *result,
    const char *details
);

/*
 * Capture a real Linux/system snapshot: process uptime, system
 * uptime, load averages when available, memory, disk space, and
 * process identity. Unsupported fields are marked unavailable,
 * never fabricated.
 */
int r2_log_system_snapshot(const char *reason);


/* Full conversation record, including exact user and assistant text. */
int64_t r2_log_conversation_turn(const char *user_text,
                                const char *assistant_text);

/*
 * Structured self-state. Confidence is 0.0..1.0, or negative if unknown.
 * Records describe R2's expressed or explicitly evidenced state, not
 * objectively measured human-like feelings.
 */
int64_t r2_log_inner_state(const char *state_kind,
                           const char *description,
                           const char *evidence,
                           double confidence,
                           const char *origin);

/* Belief ledger retains current state and full revision history in events. */
int64_t r2_log_belief(const char *belief_key,
                     const char *belief,
                     const char *evidence,
                     double confidence,
                     const char *status,
                     const char *origin);

/* Continuity ledger for projects, goals, tasks, questions, and routines. */
int64_t r2_log_continuity(const char *item_key,
                          const char *item_type,
                          const char *title,
                          const char *description,
                          const char *status,
                          const char *next_action,
                          const char *origin);

/* Link two events by a named relationship. */
int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes);

/* Hypothetical records are explicitly segregated from real events. */
int64_t r2_log_hypothetical(const char *scenario,
                            const char *assumptions,
                            const char *predicted_outcome,
                            const char *conclusion,
                            const char *origin);

/* Life Log overview. Caller frees returned text. */
char *r2_log_status_report(void);


/*
 * Query helpers return newly allocated strings. The caller must
 * free() the result. A NULL return indicates failure.
 */
char *r2_log_recent(int limit);
char *r2_log_search(const char *query, int limit);

/* Return non-zero when initialized. */
int r2_log_is_initialized(void);

/* Monotonic milliseconds since the current process began. */
uint64_t r2_log_elapsed_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* R2_LOG_H */
