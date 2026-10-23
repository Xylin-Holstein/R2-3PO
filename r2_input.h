#ifndef R2_INPUT_H
#define R2_INPUT_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    R2_INPUT_LINE_IO_ERROR = -3,
    R2_INPUT_LINE_EMBEDDED_NUL = -2,
    R2_INPUT_LINE_TOO_LONG = -1,
    R2_INPUT_LINE_EOF = 0,
    R2_INPUT_LINE_OK = 1
} R2InputLineStatus;

/*
 * Read one complete line with fixed memory. Consume the whole line even when
 * overlong, and reject embedded NUL bytes so C-string APIs cannot reinterpret
 * a binary line as a different message.
 */
static inline R2InputLineStatus
r2_read_bounded_line(FILE *stream, char *buffer, size_t max_bytes,
                     size_t *out_length)
{
    if (!stream || !buffer || !out_length) {
        errno = EINVAL;
        return R2_INPUT_LINE_IO_ERROR;
    }
    size_t used = 0;
    int saw_input = 0;
    int too_long = 0;
    int embedded_nul = 0;
    int ch;
    while ((ch = fgetc(stream)) != EOF) {
        saw_input = 1;
        if (ch == '\n')
            break;
        if (ch == '\0')
            embedded_nul = 1;
        if (used < max_bytes)
            buffer[used++] = (char)ch;
        else
            too_long = 1;
    }
    if (ch == EOF && ferror(stream))
        return R2_INPUT_LINE_IO_ERROR;
    if (!saw_input && ch == EOF)
        return R2_INPUT_LINE_EOF;
    buffer[used] = '\0';
    *out_length = used;
    if (too_long)
        return R2_INPUT_LINE_TOO_LONG;
    if (embedded_nul)
        return R2_INPUT_LINE_EMBEDDED_NUL;
    return R2_INPUT_LINE_OK;
}

#endif
