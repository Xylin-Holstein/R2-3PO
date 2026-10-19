#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>

static char *repeat_char(char ch, size_t count)
{
    char *text = malloc(count + 1);
    if (!text) return NULL;
    memset(text, ch, count);
    text[count] = '\0';
    return text;
}

static int test_valid_utf8(const unsigned char *s)
{
    while (*s) {
        if (*s < 0x80) { ++s; continue; }
        size_t continuation = (*s >= 0xC2 && *s <= 0xDF) ? 1 :
                              (*s >= 0xE0 && *s <= 0xEF) ? 2 :
                              (*s >= 0xF0 && *s <= 0xF4) ? 3 : 99;
        if (continuation == 99) return 0;
        unsigned char first = *s++;
        for (size_t i = 0; i < continuation; ++i) {
            if ((s[i] & 0xC0U) != 0x80U) return 0;
        }
        if ((first == 0xE0 && s[0] < 0xA0) ||
            (first == 0xED && s[0] >= 0xA0) ||
            (first == 0xF0 && s[0] < 0x90) ||
            (first == 0xF4 && s[0] >= 0x90)) return 0;
        s += continuation;
    }
    return 1;
}

static char *fake_associated_search(const char *query, int limit)
{
    (void)query;
    (void)limit;
    return strdup("A\xe2\x98\x83ZZZZ");
}

static void clear_test_messages(void)
{
    pthread_mutex_lock(&messages_lock);
    for (size_t i = 0; i < messages.count; ++i) {
        free(messages.items[i].role);
        free(messages.items[i].content);
        memset(&messages.items[i], 0, sizeof(messages.items[i]));
    }
    messages.count = 0;
    pthread_mutex_unlock(&messages_lock);
}

int main(void)
{
    /* Byte-based prompt budgets must not cut through a multibyte character. */
    const char unicode_prompt[] = "A\xe2\x98\x83Z"; /* A, snowman, Z */
    assert(r2_utf8_safe_prefix_length(unicode_prompt, 2) == 1);
    assert(r2_utf8_safe_prefix_length(unicode_prompt, 3) == 1);
    assert(r2_utf8_safe_prefix_length(unicode_prompt, 4) == 4);
    assert(r2_safe_prompt_prefix_length(unicode_prompt, 2, 0) == 1);
    assert(r2_safe_prompt_prefix_length(unicode_prompt, 2, 1) == 1);
    assert(message_add("user", "OLDER_USER_TURN") == 0);
    assert(message_add("assistant", "OLDER_ASSISTANT_TURN") == 0);
    assert(message_add("user", "LATEST_USER_TURN") == 0);

    char *context = r2_recent_conversation_context(NULL, 256);
    assert(context != NULL);
    char *older = strstr(context, "OLDER_USER_TURN");
    char *assistant = strstr(context, "OLDER_ASSISTANT_TURN");
    char *latest = strstr(context, "LATEST_USER_TURN");
    assert(older && assistant && latest);
    assert(older < assistant && assistant < latest);
    free(context);

    clear_test_messages();

    /* Internal action syntax alone must not pass the pre-action visible-answer gate. */
    assert(!reply_has_visible_text("[RUN] echo unsafe\n[END RUN]"));
    assert(!reply_has_visible_text("[WORLD] activity_start|session|details"));
    assert(!reply_has_visible_text("[DIARY] diary entry\n[END DIARY]"));
    assert(!reply_has_visible_text("[RUN] echo unsafe")); /* unterminated controls fail closed */
    assert(reply_has_visible_text("I can do that.\n[WORLD] activity_start|session|details"));
    assert(reply_has_visible_text("Ordinary conversational text."));

    /* Model-generated Alternate-Self IDs must reject scanf-style overflow. */
    long long parsed_id = 0, first_id = 0, second_id = 0;
    assert(r2_parse_control_positive_id(" 123\nnext marker", &parsed_id) == 0);
    assert(parsed_id == 123);
    assert(r2_parse_control_positive_id(
        " 999999999999999999999999999999999999\n", &parsed_id) != 0);
    assert(r2_parse_control_positive_id(" 12junk\n", &parsed_id) != 0);
    assert(r2_parse_control_positive_id(" -1\n", &parsed_id) != 0);
    assert(r2_parse_control_positive_id_pair(" 12 34\nnext marker",
        &first_id, &second_id) == 0);
    assert(first_id == 12 && second_id == 34);
    assert(r2_parse_control_positive_id_pair(
        " 999999999999999999999999999999 2\n",
        &first_id, &second_id) != 0);
    assert(r2_parse_control_positive_id_pair(" 12 34 extra\n",
        &first_id, &second_id) != 0);
    assert(r2_parse_control_positive_id_pair(" 12\n",
        &first_id, &second_id) != 0);

    /* Executable control turns are history metadata, not human-facing dialogue. */
    assert(message_add("assistant",
        "[RUN] echo unsafe\n[END RUN]") == 0);
    assert(message_add("assistant",
        "[HAND_REQUEST] inspect the workspace [END HAND_REQUEST]") == 0);
    assert(message_add("assistant",
        "Documentation example:\n```text\n[RUN] echo safe\n[END RUN]\n```") == 0);
    assert(message_add("user",
        "I literally saw [RUN] in a manual.") == 0);
    context = r2_recent_conversation_context(NULL, 2048);
    assert(context != NULL);
    assert(strstr(context, "echo unsafe") == NULL);
    assert(strstr(context, "inspect the workspace") == NULL);
    assert(strstr(context, "echo safe") != NULL);
    assert(strstr(context, "I literally saw [RUN] in a manual.") != NULL);
    free(context);
    clear_test_messages();

    /* Per-message excerpts must not end in an incomplete UTF-8 sequence. */
    char *utf8_edge = malloc(1003);
    assert(utf8_edge != NULL);
    memset(utf8_edge, 'x', 998);
    utf8_edge[998] = (char)0xE2;
    utf8_edge[999] = (char)0x98;
    utf8_edge[1000] = (char)0x83;
    utf8_edge[1001] = 'Z';
    utf8_edge[1002] = '\0';
    assert(message_add("user", utf8_edge) == 0);
    context = r2_recent_conversation_context(NULL, 2048);
    assert(context != NULL);
    assert(test_valid_utf8((const unsigned char *)context));
    assert(strchr(context, (char)0xE2) == NULL);
    free(context);
    clear_test_messages();
    free(utf8_edge);

    /* Shared evidence and associated-record caps must not leave partial UTF-8. */
    char evidence[] = "A\xe2\x98\x83ZZ";
    r2_bound_context_records(evidence, 2);
    assert(test_valid_utf8((const unsigned char *)evidence));
    assert(strcmp(evidence, "A") == 0);

    char keywords[MAX_MEMORY_KEYWORDS][64] = {{0}};
    snprintf(keywords[0], sizeof(keywords[0]), "signal");
    char *associated = r2_collect_associated_records(
        keywords, 1, fake_associated_search, 2);
    assert(associated != NULL);
    assert(test_valid_utf8((const unsigned char *)associated));
    assert(strcmp(associated, "A") == 0);
    free(associated);

    /* The explicit tiny-budget fallback must also preserve code-point boundaries. */
    const char tiny_unicode[] = "A\xe2\x98\x83ZZZZZZZZZZZZZZZZZZZZ";
    assert(message_add("user", tiny_unicode) == 0);
    context = r2_recent_conversation_context(NULL, 21);
    assert(context != NULL);
    assert(test_valid_utf8((const unsigned char *)context));
    assert(strchr(context, (char)0xE2) == NULL);
    assert(strlen(context) <= 21);
    free(context);
    clear_test_messages();

    char *old_user = repeat_char('O', 900);
    char *old_assistant = repeat_char('A', 900);
    char *oversized_latest = repeat_char('N', 600);
    assert(old_user && old_assistant && oversized_latest);

    assert(message_add("user", old_user) == 0);
    assert(message_add("assistant", old_assistant) == 0);
    assert(message_add("user", "LATEST_USER_TURN_SHOULD_SURVIVE") == 0);

    context = r2_recent_conversation_context(NULL, 256);
    assert(context != NULL);
    assert(strstr(context, "LATEST_USER_TURN_SHOULD_SURVIVE") != NULL);
    assert(strstr(context, "OOOO") == NULL);
    assert(strlen(context) <= 256);
    free(context);

    assert(message_add("user", oversized_latest) == 0);
    context = r2_recent_conversation_context(NULL, 256);
    assert(context != NULL);
    assert(strstr(context, "NNNN") != NULL);
    assert(strstr(context, "[truncated]") != NULL);
    assert(strlen(context) <= 256);
    free(context);

    /* Small caller budgets must remain hard limits, not be inflated to 256. */
    context = r2_recent_conversation_context(NULL, 64);
    assert(context != NULL);
    assert(strstr(context, "NNNN") != NULL);
    assert(strstr(context, "[truncated]") != NULL);
    assert(strlen(context) <= 64);
    free(context);
    context = r2_recent_conversation_context(NULL, 0);
    assert(context != NULL && context[0] == '\0');
    free(context);

    free(old_user);
    free(old_assistant);
    free(oversized_latest);
    clear_test_messages();
    puts("Recent conversation context runtime regression passed.");
    return 0;
}
