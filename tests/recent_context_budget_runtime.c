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
