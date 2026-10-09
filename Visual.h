#ifndef R2_VISUAL_H
#define R2_VISUAL_H

#include <stdint.h>
#include "Eyes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * R2 Visual Experience Library + Ollama vision-model bridge.
 * Images and descriptions are stored separately from diary and
 * conversational-memory tables.
 */
int r2_visual_init(const char *database_path, const char *library_directory);
void r2_visual_shutdown(void);
int r2_visual_is_initialized(void);
int r2_visual_set_model(const char *model);
const char *r2_visual_model_name(void);

/* Analyze one real Eyes frame. Returns a malloc-owned description. */
char *r2_visual_analyze_frame(const R2VisionFrame *frame,
                              const char *source,
                              const char *question);

/* Read recent visual experiences as context; caller frees result. */
char *r2_visual_recent(int limit);
char *r2_visual_search(const char *query, int limit);

#ifdef __cplusplus
}
#endif
#endif
