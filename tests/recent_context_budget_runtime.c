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

int main(void)
{
    char *old_user = repeat_char('O', 900);
    char *old_assistant = repeat_char('A', 900);
    char *oversized_latest = repeat_char('N', 600);
    assert(old_user && old_assistant && oversized_latest);

    assert(message_add("user", old_user) == 0);
    assert(message_add("assistant", old_assistant) == 0);
    assert(message_add("user", "LATEST_USER_TURN_SHOULD_SURVIVE") == 0);

    char *context = r2_recent_conversation_context(NULL, 256);
    assert(context != NULL);
    assert(strstr(context, "LATEST_USER_TURN_SHOULD_SURVIVE") != NULL);
    assert(strlen(context) <= 256);
    free(context);

    assert(message_add("user", oversized_latest) == 0);
    context = r2_recent_conversation_context(NULL, 256);
    assert(context != NULL);
    assert(strstr(context, "NNNN") != NULL);
    assert(strstr(context, "[truncated]") != NULL);
    assert(strlen(context) <= 256);
    free(context);

    free(old_user);
    free(old_assistant);
    free(oversized_latest);
    puts("Recent conversation context runtime regression passed.");
    return 0;
}
