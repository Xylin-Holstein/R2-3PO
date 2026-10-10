#ifndef R2_H
#define R2_H

#include <stddef.h>

/*
 * ============================================================
 * R2-3PO CORE PUBLIC INTERFACE
 * ============================================================
 *
 * r2.c owns the actual implementation:
 *
 *   - Ollama / Gemma 4 multimodal conversation and vision
 *   - conversation
 *   - memory
 *   - relevant-memory retrieval
 *   - startup memory loading
 *   - autonomous thinking
 *   - tools
 *   - diary integration
 *   - Eyes
 *   - Ears
 *
 * This header is the PUBLIC bridge used by other R2 components,
 * especially shell.c.
 *
 * Do NOT put implementation here.
 * Do NOT duplicate R2 subsystems here.
 * ============================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Single source of truth for every local Ollama inference in R2. */
#define R2_OLLAMA_MODEL "gemma4:e2b"

/* Bounded inference settings for the local 16 GB RAM target. */
#define R2_OLLAMA_NUM_CTX 8192
#define R2_OLLAMA_NUM_BATCH 256


/* ============================================================
   CORE LIFECYCLE
   ============================================================ */

/*
 * Initialize the R2 core.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 *
 * This should eventually contain/use the same initialization
 * sequence currently performed by r2.c's main().
 */
int r2_init(void);


/*
 * Shut down the R2 core cleanly.
 *
 * This allows the shell to request a normal R2 shutdown without
 * directly manipulating R2's internal threads/resources.
 */
void r2_shutdown(void);


/*
 * Returns non-zero while R2 is shutting down.
 */
int r2_is_shutting_down(void);

/*
 * Shared Ollama request gate. Vision uses this same gate as text generation
 * so the single local model is never asked to run competing inference jobs.
 * Vision requests are opportunistic and must yield when a foreground turn waits.
 */
int r2_ollama_vision_request_begin(void);
void r2_ollama_vision_request_end(void);
int r2_ollama_vision_request_should_abort(void);


/* ============================================================
   CONVERSATION
   ============================================================ */

/*
 * Send a message to R2 and receive its response.
 *
 * This is the public equivalent of entering a normal message
 * into R2's existing conversation system.
 *
 * The implementation in r2.c should continue to use:
 *
 *   message_add()
 *   chat_with_relevant_memories()
 *   process_tools()
 *   memory_decision()
 *   save_memory()
 *
 * so the shell does NOT create a second conversation system.
 *
 * Returned string is allocated by the R2 core and must be freed
 * by the caller with free().
 */
char *r2_talk(const char *message);


/*
 * Run R2's normal interactive conversation loop.
 *
 * This exists so the shell can enter normal conversation mode
 * without duplicating the conversation implementation.
 */
int r2_conversation(void);


/* ============================================================
   THINKING
   ============================================================ */

/*
 * Ask R2 to perform one autonomous thinking cycle immediately.
 *
 * This uses R2's existing autonomous-thinking system.
 */
int r2_think(void);


/*
 * Start R2's autonomous thinking thread.
 *
 * Normally this is started during core initialization.
 */
int r2_start_thinking(void);


/*
 * Stop R2's autonomous thinking thread.
 */
void r2_stop_thinking(void);


/*
 * Returns non-zero if autonomous thinking is currently active.
 */
int r2_thinking_active(void);


/* ============================================================
   MEMORY
   ============================================================ */

/*
 * Retrieve memories relevant to a query.
 *
 * This uses R2's existing relevant-memory retrieval system.
 *
 * The shell can use this for commands such as:
 *
 *   memory search <query>
 *
 * Returned string must be freed by the caller.
 */
char *r2_retrieve_memories(const char *query);

/* Copy a bounded recent active-conversation context, excluding the current user
 * message when it matches exclude_latest. Caller frees the result. */
char *r2_recent_conversation_context(const char *exclude_latest, size_t max_chars);


/*
 * Save a memory through R2's existing memory system.
 *
 * category may be NULL if no category is required.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int r2_save_memory(const char *memory, const char *category);


/*
 * Load the startup memory set.
 *
 * R2 normally loads the newest startup memories during
 * initialization. This public function allows the shell to
 * request the operation without accessing the database directly.
 */
int r2_load_startup_memories(void);


/*
 * Return the number of memories currently known to the core,
 * when available.
 */
long r2_memory_count(void);


/* ============================================================
   TOOLS
   ============================================================ */

/*
 * Process an R2 tool command through the existing R2 tool system.
 *
 * The shell must not implement a second tool executor.
 */
char *r2_process_tools(const char *input);

/* Generate one bounded completion through the shared Ollama model without
 * adding a fake user/assistant turn to R2's conversation history. Caller frees. */
char *r2_model_generate(const char *system_prompt, const char *user_prompt,
                        int max_tokens);


/* ============================================================
   DIARY
   ============================================================ */

/*
 * Trigger a diary/autonomous-writing operation through the
 * existing diary system.
 *
 * r2.c remains responsible for connecting this to the existing
 * r2_diary subsystem.
 */
int r2_write_diary(void);

/* Simulate a sleep transition and generate/store a private dream report. */
int r2_sleep_and_dream(double hours);
int r2_eat_and_learn(const char *food, double fullness);
int r2_eat_fridge_and_learn(const char *food, double fullness);


/*
 * Return whether the diary subsystem is currently available.
 */
int r2_diary_active(void);


/* ============================================================
   STATUS
   ============================================================ */

/*
 * Print a human-readable R2 status report.
 *
 * Intended for:
 *
 *   status
 *
 * from shell.c.
 */
void r2_status(void);


/*
 * Return whether the R2 core has been initialized.
 */
int r2_is_initialized(void);


/*
 * Return the model currently being used.
 *
 * Example:
 *
 *   Gemma 4 E2B
 *
 * The returned pointer is owned by R2 and must NOT be freed.
 */
const char *r2_model_name(void);


/* ============================================================
   EYES
   ============================================================ */

/*
 * Tell the R2 core that the shell is requesting visual
 * functionality.
 *
 * The implementation must call the EXISTING Eyes subsystem.
 *
 * These functions are intentionally kept as bridges so shell.c
 * never needs to know how Eyes internally works.
 */
int r2_eyes_start(void);
int r2_eyes_stop(void);
int r2_eyes_status(void);

/* ============================================================
   VISUAL EXPERIENCE LIBRARY / VISION MODEL
   ============================================================ */

/* Analyze the current frame; opens the camera only for explicit vision requests. */
int r2_vision_available(void);
const char *r2_vision_model_name(void);
int r2_vision_watch_start(void);
int r2_vision_watch_stop(void);
int r2_vision_watch_active(void);
char *r2_vision_see(const char *question);
char *r2_vision_recent(int limit);
char *r2_vision_search(const char *query, int limit);
int r2_vision_set_model(const char *model);
int r2_vision_open_vlc(void);
int r2_vision_open_file(const char *path);
int r2_vision_close(void);


/* ============================================================
   EARS
   ============================================================ */

/*
 * Tell the R2 core that the shell is requesting audio/listening
 * functionality.
 *
 * The implementation must call the EXISTING Ears subsystem.
 */
int r2_ears_start(void);
int r2_ears_stop(void);
int r2_ears_status(void);


/* ============================================================
   WATCH SUPPORT
   ============================================================ */

/*
 * Watch is the NEW subsystem.
 *
 * It will coordinate:
 *
 *   - microphone
 *   - system/desktop audio
 *   - VLC detection
 *   - VLC/video observation
 *
 * Watch itself belongs in shell/watch code.
 *
 * These functions allow the core to expose its state without
 * duplicating Eyes or Ears.
 */


/*
 * Returns non-zero if Watch mode is currently active.
 */
int r2_watch_active(void);


/*
 * Request Watch mode to begin.
 *
 * The actual Watch implementation will live outside the core.
 */
int r2_watch_start(void);


/*
 * Stop Watch mode.
 */
int r2_watch_stop(void);


/*
 * Print Watch status.
 */
void r2_watch_status(void);


/* ============================================================
   SHELL / COMMAND SUPPORT
   ============================================================ */

/*
 * Request a normal R2 shutdown.
 *
 * This is intentionally separate from r2_shutdown() so shell
 * commands can request shutdown without directly manipulating
 * the core lifecycle.
 */
void r2_request_shutdown(void);


/*
 * Request a restart/reboot of the R2 program.
 *
 * This is a program-level restart, NOT an operating-system
 * reboot.
 */
int r2_request_restart(void);


/* ============================================================
   THREAD / STATE INFORMATION
   ============================================================ */

/*
 * Return the current number of active R2 worker threads when
 * available.
 */
int r2_worker_count(void);


/*
 * Return the configured autonomous-thinking interval in seconds.
 */
int r2_think_interval(void);


/* ============================================================
   DEBUG / DIAGNOSTICS
   ============================================================ */

/*
 * Print diagnostic information about R2's internal systems.
 *
 * Intended for shell:
 *
 *   debug
 *   diagnostics
 *
 * This must report actual state rather than simulated values.
 */
void r2_diagnostics(void);


/* ============================================================
   END PUBLIC INTERFACE
   ============================================================ */

#ifdef __cplusplus
}
#endif

#endif /* R2_H */
