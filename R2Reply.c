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

/* Track nested private-tag types, not just repetitions of the outer tag.
   A malformed crossing such as <think><analysis></think>...</analysis></think>
   must not close the outer block early and expose the intervening text. */
static const char *find_hidden_close_balanced(const char *p,
                                               const HiddenBlock *block)
{
    size_t capacity = strlen(p) + 1;
    if (capacity > (size_t)-1 / sizeof(const HiddenBlock *)) return NULL;
    const HiddenBlock **stack = malloc(capacity * sizeof(*stack));
    if (!stack) return NULL;

    size_t depth = 1;
    stack[0] = block;
    while (*p) {
        const HiddenBlock *opened = NULL;
        const char *after_open = find_hidden_open(p, &opened);
        if (after_open) {
            if (depth >= capacity) {
                free(stack);
                return NULL;
            }
            stack[depth++] = opened;
            p = after_open;
            continue;
        }

        size_t close_len = 0;
        if (is_hidden_close(p, &close_len)) {
            const char *expected = stack[depth - 1]->close;
            if (!strncasecmp(p, expected, strlen(expected))) {
                --depth;
                if (depth == 0) {
                    free(stack);
                    return p;
                }
            }
            /* A close for an outer tag cannot close through an unclosed
               nested tag. Ignore it until the nested block is closed. */
            p += close_len;
            continue;
        }
        ++p;
    }
    free(stack);
    return NULL;
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
            const char *close = find_hidden_close_balanced(after_open, block);
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

    /* Removing a tagged private block can reveal an untagged scratch preamble
       that was not at byte zero of the model output. Check the visible result
       again so "<think>...</think>We need to answer..." cannot leak. */
    const char *visible = clean;
    while (*visible && isspace((unsigned char)*visible)) ++visible;
    int visible_looks_private =
        !strncasecmp(visible, "analysis:", 9) ||
        !strncasecmp(visible, "reasoning:", 10) ||
        !strncasecmp(visible, "thinking:", 9);
    for (size_t i = 0; !visible_looks_private && scratch_prefixes[i]; ++i) {
        size_t n = strlen(scratch_prefixes[i]);
        if (!strncasecmp(visible, scratch_prefixes[i], n))
            visible_looks_private = 1;
    }
    if (visible_looks_private) {
        const char *final = strcasestr(visible, "\nfinal:");
        if (!final) final = strcasestr(visible, "\n\nfinal:");
        if (!final) {
            free(clean);
            return NULL;
        }
        const char *final_content = final + strlen("\nfinal:");
        memmove(clean, final_content, strlen(final_content) + 1);
        trim_reply(clean);
    }

    if (!*clean) {
        free(clean);
        return NULL;
    }
    return clean;
}
