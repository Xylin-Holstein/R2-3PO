#ifndef R2_SHELL_H
#define R2_SHELL_H

/*
 * ============================================================
 * R2-3PO SHELL PUBLIC INTERFACE
 * ============================================================
 *
 * shell.c provides the command-line interface for R2-3PO.
 *
 * The shell communicates with the R2 core through r2.h.
 *
 * It does NOT replace:
 *
 *   - R2 core
 *   - Memory
 *   - Ollama
 *   - Thinking
 *   - Diary
 *   - Eyes
 *   - Ears
 *
 * Watch is the only new subsystem controlled by the shell.
 *
 * ============================================================
 */

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
   SHELL LIFECYCLE
   ============================================================ */

/*
 * Start the interactive R2 shell.
 *
 * This function does not initialize the R2 core itself.
 * R2's core must already be initialized before calling it.
 *
 * Returns:
 *   0  = normal exit
 *  -1  = shell failure
 */
int r2_shell_run(void);


/*
 * Request the shell to stop.
 *
 * This stops the shell interface without directly replacing
 * the R2 core shutdown system.
 */
void r2_shell_shutdown(void);


/*
 * Returns non-zero while the shell is running.
 */
int r2_shell_is_running(void);


/* ============================================================
   WATCH STATE
   ============================================================ */

/*
 * Returns non-zero when Watch mode is currently active.
 */
int r2_shell_watch_is_running(void);


/* ============================================================
   END SHELL INTERFACE
   ============================================================ */

#ifdef __cplusplus
}
#endif

#endif /* R2_SHELL_H */
