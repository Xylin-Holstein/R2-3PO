#define _GNU_SOURCE
#include "R2Reply.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *open;
    const char *close;
} HiddenBlock;

static const HiddenBlock hidden_blocks[] = {
    {"<think>", "</think>"},
    {"<analysis>", "</analysis>"},
    {"<reasoning>", "</reasoning>"},
    {"<scratchpad>", "</scratchpad>"},
    {"<internal>", "</internal>"},
    {"<private_thought>", "</private_thought>"},
    {"<|thinking|>", "<|/thinking|>"},
    {"<|analysis|>", "<|/analysis|>"}
};

static const char *find_hidden_open(const char *p, const HiddenBlock **found)
{
    for (size_t i = 0; i < sizeof(hidden_blocks) / sizeof(hidden_blocks[0]); ++i) {
        size_t n = strlen(hidden_blocks[i].open);
        if (!strncasecmp(p, hidden_blocks[i].open, n)) {
            *found = &hidden_blocks[i];
            return p + n;
        }
    }
    return NULL;
}

static int is_hidden_close(const char *p, size_t *length)
{
    for (size_t i = 0; i < sizeof(hidden_blocks) / sizeof(hidden_blocks[0]); ++i) {
        size_t n = strlen(hidden_blocks[i].close);
        if (!strncasecmp(p, hidden_blocks[i].close, n)) {
            if (length) *length = n;
            return 1;
        }
    }
    return 0;
}

static char *trim_reply(char *text)
{
    char *start = text;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);

    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
    return text;
}

char *r2_reply_sanitize(const char *reply)
{
    if (!reply) return NULL;
    size_t input_len = strlen(reply);
    if (!input_len) return NULL;

    /* Some local models emit labeled analysis/final sections rather than XML
       blocks. If the response begins with a private analysis label and has a
       distinct final label, expose only the final section. */
    const char *start = reply;
    while (*start && isspace((unsigned char)*start)) ++start;
    int labeled_private = !strncasecmp(start, "analysis:", 9) ||
                          !strncasecmp(start, "reasoning:", 10) ||
                          !strncasecmp(start, "thinking:", 9);
    static const char *const scratch_prefixes[] = {
        "we need answer", "we need to answer", "we need respond",
        "we need to respond", "we should answer", "we should respond",
        "we need provide", "we need to provide", "we should provide",
        "let's craft", "let us craft", "let me formulate",
        "i need to answer", "i should answer", "i will respond",
        "i'll answer", "the user asks", "the user wants",
        "we need make sure", "we need ensure", NULL
    };
    int looks_like_scratch = labeled_private;
    for (size_t i = 0; !looks_like_scratch && scratch_prefixes[i]; ++i) {
        size_t n = strlen(scratch_prefixes[i]);
        if (!strncasecmp(start, scratch_prefixes[i], n))
            looks_like_scratch = 1;
    }
    if (looks_like_scratch) {
        const char *final = strcasestr(start, "\nfinal:");
        if (!final) final = strcasestr(start, "\n\nfinal:");
        if (!final) return NULL;
        start = final + strlen("\nfinal:");
    }

    char *clean = malloc(strlen(start) + 1);
    if (!clean) return NULL;

    size_t src = 0, dst = 0;
    size_t len = strlen(start);
    while (src < len) {
        const HiddenBlock *block = NULL;
        const char *after_open = find_hidden_open(start + src, &block);
        if (after_open) {
            const char *close = strcasestr(after_open, block->close);
            if (!close) {
                /* An unclosed private block is not safe to display. */
                break;
            }
            src = (size_t)(close - start) + strlen(block->close);
            continue;
        }

        size_t close_len = 0;
        if (is_hidden_close(start + src, &close_len)) {
            src += close_len;
            continue;
        }

        clean[dst++] = start[src++];
    }
    clean[dst] = '\0';
    trim_reply(clean);

    if (!*clean) {
        free(clean);
        return NULL;
    }
    return clean;
}
