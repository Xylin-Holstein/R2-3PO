#ifndef R2_SOUNDS_H
#define R2_SOUNDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uses R2_FX_DIR when set; otherwise the established R2 runtime FX directory. */
#define R2_SOUNDS_DEFAULT_FX_DIR "/home/x/R2_Home/R2_sounds/FX"

/* Select an existing named MP3 using filename tokens; never synthesizes files. */
int r2_sounds_select_file(const char *fx_dir, const char *kind,
                          const char *state, char *out, size_t out_cap);

/* Queue one short effect asynchronously. kind is "beep" or "whistle". */
int r2_sounds_play(const char *kind, const char *state);
/* Stop/reap any active short effect during orderly core shutdown. */
void r2_sounds_shutdown(void);

/* Remove private [R2_SOUND:kind:state] markers and play at most one effect.
   Caller frees the returned reply. Unknown/malformed markers are stripped. */
char *r2_sounds_process_reply(const char *reply);

#ifdef __cplusplus
}
#endif

#endif
