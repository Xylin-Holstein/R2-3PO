#ifndef R2_DIARY_H
#define R2_DIARY_H

#include <stddef.h>

/*
 * ============================================================
 * R2 FILESYSTEM LAYOUT
 * ============================================================
 *
 * /home/x/R2_Home/
 *
 * ├── R2/
 * │   ├── r2.c
 * │   ├── r2_diary.c
 * │   ├── r2_diary.h
 * │   ├── r2_memory.db
 * │   └── r2_original_conversation.txt
 * │
 * └── R2_Diary/
 *     └── YYYY-MM-DD.md
 *
 * R2_ROOT      = entire R2 workspace
 * R2_HOME      = R2's kernel/code directory
 * R2_WORKSPACE = root exposed to R2's filesystem tools
 * R2_DIARY_DIR = private diary directory
 *
 * ============================================================
 */

#define R2_ROOT "/home/x/R2_Home"
#define R2_HOME R2_ROOT "/R2"
#define R2_WORKSPACE R2_ROOT

#ifndef R2_DIARY_DATABASE
#define R2_DIARY_DATABASE R2_HOME "/r2_memory.db"
#endif
#define R2_DIARY_DIR R2_ROOT "/R2_Diary"


/* ============================================================
   DIARY LIFECYCLE
   ============================================================ */

int r2_diary_init(void);

void r2_diary_shutdown(void);

/* Reconcile existing and pending diary entries with Life Log/memory.
 * Work is bounded per call; returns linked-entry count or -1 on error. */
int r2_diary_reconnect_history(int limit);


/* ============================================================
   DIARY OPERATIONS
   ============================================================ */

int r2_diary_write(
    const char *entry
);

char *r2_diary_recent(
    int count
);

char *r2_diary_search(
    const char *query,
    int limit
);


/* ============================================================
   REFLECTION
   ============================================================ */

char *r2_diary_build_reflection_context(
    int limit
);


/* ============================================================
   WORKSPACE
   ============================================================ */

char *r2_workspace_list(
    const char *relative
);

char *r2_workspace_read(
    const char *relative,
    size_t *size
);

char *r2_workspace_read_chunk(
    const char *relative,
    long offset,
    size_t length,
    size_t *out_size,
    long *next_offset
);

char *r2_workspace_search(
    const char *relative,
    const char *query
);


/* ============================================================
   TOOL INTERFACE
   ============================================================ */

char *r2_diary_tool(
    const char *request,
    const char *reply
);


#endif /* R2_DIARY_H */
