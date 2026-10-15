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
static const char *find_hidden_close_balanced_until(const char *p,
                                                     const HiddenBlock *block,
                                                     const char *limit)
{
    /*
       Bound stack memory by actual nested tag openings. When sanitizing code,
       limit the search to that code span so a literal opener cannot pair with
       a closing token from unrelated prose or another example.
    */
    size_t nested_open_count = 0;
    const char *scan = p;
    while (*scan && (!limit || scan < limit)) {
        const HiddenBlock *nested = NULL;
        const char *after_open = find_hidden_open(scan, &nested);
        if (after_open && (!limit || after_open <= limit)) {
            if (nested_open_count == (size_t)-1) return NULL;
            ++nested_open_count;
            scan = after_open;
        } else {
            ++scan;
        }
    }

    if (nested_open_count == (size_t)-1) return NULL;
    size_t capacity = nested_open_count + 1;
    if (capacity > (size_t)-1 / sizeof(const HiddenBlock *)) return NULL;
    const HiddenBlock **stack = malloc(capacity * sizeof(*stack));
    if (!stack) return NULL;

    size_t depth = 1;
    stack[0] = block;
    while (*p && (!limit || p < limit)) {
        const HiddenBlock *opened = NULL;
        const char *after_open = find_hidden_open(p, &opened);
        if (after_open && (!limit || after_open <= limit)) {
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
            if (limit && (size_t)(limit - p) < close_len) {
                ++p;
                continue;
            }
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

static const char *find_hidden_close_balanced(const char *p,
                                               const HiddenBlock *block)
{
    return find_hidden_close_balanced_until(p, block, NULL);
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

/* Markdown code is user-facing syntax, not permission to expose private
   contents. Preserve tag delimiters in examples while redacting their body. */
static const char *reply_find_matching_ticks(const char *text, size_t length,
                                                size_t after, size_t run_length)
{
    size_t pos = after;
    while (pos < length) {
        if (text[pos] != '`') {
            ++pos;
            continue;
        }
        size_t run = 1;
        while (pos + run < length && text[pos + run] == '`') ++run;
        if (run == run_length) return text + pos;
        pos += run;
    }
    return NULL;
}

static int reply_fence_line(const char *line, size_t length,
                            char expected_char, size_t expected_length,
                            int closing, char *found_char, size_t *found_length)
{
    size_t pos = 0;
    while (pos < length && line[pos] == ' ' && pos < 4) ++pos;
    if (pos > 3 || pos >= length ||
        (line[pos] != '`' && line[pos] != '~'))
        return 0;

    char ch = line[pos];
    size_t run = 0;
    while (pos + run < length && line[pos + run] == ch) ++run;
    if (run < 3) return 0;

    if (closing) {
        if (ch != expected_char || run < expected_length) return 0;
        pos += run;
        while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        if (pos != length) return 0;
    } else if (ch == '`') {
        for (size_t i = pos + run; i < length; ++i)
            if (line[i] == '`') return 0;
    }

    if (found_char) *found_char = ch;
    if (found_length) *found_length = run;
    return 1;
}

static const char *reply_find_fence_close(const char *text, size_t length,
                                          size_t from, char fence_char,
                                          size_t fence_length)
{
    size_t pos = from;
    while (pos < length) {
        if (pos != 0 && text[pos - 1] != '\n') {
            ++pos;
            continue;
        }
        size_t line_end = pos;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t check_length = line_end - pos;
        if (check_length && text[pos + check_length - 1] == '\r')
            --check_length;
        if (reply_fence_line(text + pos, check_length,
                             fence_char, fence_length, 1, NULL, NULL))
            return text + pos;
        pos = line_end < length ? line_end + 1 : line_end;
    }
    return text + length;
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

    size_t len = strlen(start);
    size_t block_count = 0;
    for (size_t i = 0; i < len; ++i) {
        if (start[i] == '<') {
            const HiddenBlock *ignored = NULL;
            if (find_hidden_open(start + i, &ignored)) ++block_count;
        }
    }
    /* Code examples keep their tag delimiters but replace the body with a
       fixed placeholder, so reserve space for every possible expansion. */
    if (block_count > ((size_t)-1 - len - 1) / 32) return NULL;
    char *clean = malloc(len + block_count * 32 + 1);
    if (!clean) return NULL;

    size_t src = 0, dst = 0;
    size_t inline_ticks = 0;
    int in_fence = 0;
    char fence_char = 0;
    size_t fence_length = 0;
    size_t fence_close_after = (size_t)-1;
    while (src < len) {
        if (fence_close_after != (size_t)-1 && src >= fence_close_after) {
            in_fence = 0;
            fence_close_after = (size_t)-1;
        }
        /* Recognize Markdown fences only at line boundaries and outside
           inline code; private tag bodies are still redacted within examples. */
        if (inline_ticks == 0 && (src == 0 || start[src - 1] == '\n')) {
            size_t line_end = src;
            while (line_end < len && start[line_end] != '\n') ++line_end;
            size_t line_length = line_end - src;
            size_t check_length = line_length;
            if (check_length && start[src + check_length - 1] == '\r')
                --check_length;
            size_t after_line = line_end < len ? line_end + 1 : line_end;
            if (in_fence) {
                char ignored_char = 0;
                size_t ignored_length = 0;
                if (reply_fence_line(start + src, check_length,
                                     fence_char, fence_length, 1,
                                     &ignored_char, &ignored_length))
                    fence_close_after = after_line;
            }
            char opening_char = 0;
            size_t opening_length = 0;
            if (!in_fence &&
                reply_fence_line(start + src, check_length, 0, 0, 0,
                                 &opening_char, &opening_length)) {
                size_t indent = 0;
                while (indent < check_length && start[src + indent] == ' ')
                    ++indent;
                size_t prefix_length = indent + opening_length;
                memcpy(clean + dst, start + src, prefix_length);
                dst += prefix_length;
                in_fence = 1;
                fence_char = opening_char;
                fence_length = opening_length;
                src += prefix_length;
                continue;
            }
        }

        if (!in_fence && start[src] == '`') {
            size_t run = 1;
            while (src + run < len && start[src + run] == '`') ++run;
            if (inline_ticks == 0) {
                if (reply_has_matching_ticks(start, len, src + run, run))
                    inline_ticks = run;
            } else if (inline_ticks == run) {
                inline_ticks = 0;
            }
            memcpy(clean + dst, start + src, run);
            src += run;
            dst += run;
            continue;
        }

        int inside_code = in_fence || inline_ticks != 0;
        const HiddenBlock *block = NULL;
        const char *after_open = find_hidden_open(start + src, &block);
        if (after_open) {
            const char *close = find_hidden_close_balanced(after_open, block);
            if (!close) {
                if (inside_code) {
                    size_t open_len = (size_t)(after_open - (start + src));
                    memcpy(clean + dst, start + src, open_len);
                    dst += open_len;
                    static const char omitted[] = "[unclosed content omitted]";
                    memcpy(clean + dst, omitted, sizeof(omitted) - 1);
                    dst += sizeof(omitted) - 1;
                }
                /* An unclosed private block is never displayed verbatim. */
                break;
            }
            if (inside_code) {
                size_t open_len = (size_t)(after_open - (start + src));
                memcpy(clean + dst, start + src, open_len);
                dst += open_len;
                static const char omitted[] = "[content omitted]";
                memcpy(clean + dst, omitted, sizeof(omitted) - 1);
                dst += sizeof(omitted) - 1;
                size_t close_len = strlen(block->close);
                memcpy(clean + dst, close, close_len);
                dst += close_len;
            }
            src = (size_t)(close - start) + strlen(block->close);
            continue;
        }

        size_t close_len = 0;
        if (is_hidden_close(start + src, &close_len) && !inside_code) {
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
