#ifndef R2_REPLY_H
#define R2_REPLY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return a newly allocated user-facing reply with explicit private reasoning
   blocks removed. Returns NULL if no user-facing text remains. */
char *r2_reply_sanitize(const char *reply);

#ifdef __cplusplus
}
#endif

#endif
