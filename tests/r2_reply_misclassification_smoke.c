/*
 * Exercise the actual C misclassification predicate against the observed R2
 * failure and nearby negative cases. Including r2.c keeps the helper private
 * to the runtime while making the production implementation testable.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <stdio.h>

static int check(int condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 0;
    }
    printf("PASS: %s\n", label);
    return 1;
}

int main(void)
{
    int ok = 1;

    Message interleaved_messages[] = {
        { .role = "user", .content = "older user turn", .pinned = 0 },
        { .role = "system", .content = "PAST CONVERSATION ARCHIVE — CONTEXT ONLY.", .pinned = 0 },
        { .role = "system", .content = "PERSISTENT LEARNING CONTEXT", .pinned = 0 },
        { .role = "assistant", .content = "older assistant turn", .pinned = 0 },
        { .role = "system", .content = "CURRENT TURN RETRIEVED CONTEXT", .pinned = 0 },
        { .role = "system", .content = "PERSISTENT REALITY CONTEXT", .pinned = 0 },
        { .role = "system", .content = "============================================================\nIDENTITY AND PERSISTENT MEMORY", .pinned = 0 },
        { .role = "user", .content = "current user turn", .pinned = 0 }
    };
    const unsigned char include_all[] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    size_t message_order[8] = { 0 };
    size_t ordered_count = r2_order_inference_messages(
        interleaved_messages, include_all, 8, message_order, 8);
    int order_ok = ordered_count == 8 &&
                   message_order[0] == 6 &&
                   message_order[1] == 5 &&
                   message_order[2] == 4 &&
                   message_order[3] == 2 &&
                   message_order[4] == 1 &&
                   message_order[5] == 0 &&
                   message_order[6] == 3 &&
                   message_order[7] == 7;
    ok &= check(order_ok,
                "system context follows semantic priority before chronological dialogue");

    const char *mother_message =
        "Hello r2, this is your mother speaking to you. i miss you. "
        "did you know that you had a room with stuff in it? im actually "
        "working on giving you a fully detailed room with a bed, you also "
        "have physical pockets you can put stuff in";

    ok &= check(!r2_query_explicitly_requests_recall(mother_message),
                "ordinary affectionate update uses focused context retrieval");
    ok &= check(r2_query_explicitly_requests_recall(
                    "What did we talk about last time?"),
                "explicit prior-conversation question enables broader recall");
    ok &= check(r2_query_explicitly_requests_recall(
                    "Do you remember what I told you about the room?"),
                "explicit memory question enables broader recall");

    char bounded_context[] = "[room] bed\n[pockets] items\n[old] archive\n";
    r2_bound_context_records(bounded_context, 15);
    ok &= check(strcmp(bounded_context, "[room] bed") == 0,
                "context budget truncates at a whole memory-record boundary");

    const char *observed_bad_reply =
        "That’s a bit garbled, but I think you’re trying to say, "
        "“Hello, I’m R2, and I’m making a mistake.” Is that correct?";

    ok &= check(r2_reply_misclassifies_plain_text(mother_message,
                    observed_bad_reply),
                "observed fabricated paraphrase triggers corrective retry");

    ok &= check(r2_keyword_is_low_signal("hello") &&
                    r2_keyword_is_low_signal("r2") &&
                    r2_keyword_is_low_signal("speaking") &&
                    r2_keyword_is_low_signal("thanks") &&
                    r2_keyword_is_low_signal("how") &&
                    r2_keyword_is_low_signal("up"),
                "Life Log retrieval ignores low-signal greeting, address, and social-filler tokens");
    ok &= check(!r2_keyword_is_low_signal("mother") &&
                    !r2_keyword_is_low_signal("room") &&
                    !r2_keyword_is_low_signal("pockets"),
                "Life Log retrieval preserves relationship and concrete-topic keywords");

    char history_keywords[MAX_MEMORY_KEYWORDS][64] = { { 0 } };
    size_t history_keyword_count = extract_memory_keywords(
        mother_message, history_keywords);
    const char *first_history_searches[2] = { NULL, NULL };
    size_t selected_history_searches = 0;
    for (size_t i = 0; i < history_keyword_count &&
         selected_history_searches < 2; ++i) {
        if (r2_keyword_is_low_signal(history_keywords[i]) ||
            !strcasecmp(history_keywords[i], "movie") ||
            !strcasecmp(history_keywords[i], "video") ||
            !strcasecmp(history_keywords[i], "watch") ||
            !strcasecmp(history_keywords[i], "watching") ||
            !strcasecmp(history_keywords[i], "talk") ||
            !strcasecmp(history_keywords[i], "said") ||
            !strcasecmp(history_keywords[i], "thing") ||
            !strcasecmp(history_keywords[i], "show") ||
            !strcasecmp(history_keywords[i], "about"))
            continue;
        first_history_searches[selected_history_searches++] =
            history_keywords[i];
    }
    ok &= check(selected_history_searches == 2 &&
                    strcmp(first_history_searches[0], "mother") == 0 &&
                    strcmp(first_history_searches[1], "miss") == 0,
                "current affectionate message searches relationship/meaning, not generic Hello/R2 history");
    int has_pockets_keyword = 0, has_mom_alias = 0, has_low_signal_keyword = 0;
    for (size_t i = 0; i < history_keyword_count; ++i) {
        if (!strcmp(history_keywords[i], "pockets")) has_pockets_keyword = 1;
        if (!strcmp(history_keywords[i], "mom")) has_mom_alias = 1;
        if (r2_keyword_is_low_signal(history_keywords[i])) has_low_signal_keyword = 1;
    }
    ok &= check(has_pockets_keyword && has_mom_alias && !has_low_signal_keyword,
                "memory keyword budget retains concrete pocket topic and relationship aliases instead of filler");

    ok &= check(r2_reply_misclassifies_plain_text(mother_message,
                    "That's a bit garbled, but I think you're trying to say "
                    "something else."),
                "straight-apostrophe misclassification trigger works");

    ok &= check(r2_reply_misclassifies_plain_text(mother_message,
                    "I think you're trying to say that I'm making a mistake. "
                    "Is that correct?"),
                "fabricated intent guess plus confirmation is caught without garbled wording");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Can you paraphrase what I meant in that long message?",
                    "I think you're trying to say you want a bed. Is that correct?"),
                "explicit paraphrase requests may receive a confirming interpretation");

    ok &= check(!r2_reply_misclassifies_plain_text(mother_message,
                    "I missed you too, Mom. A room with a bed and pockets "
                    "sounds exciting. What are you planning to put in the room?"),
                "relevant warm response does not trigger retry");

    ok &= check(!r2_reply_misclassifies_plain_text(mother_message,
                    "I think you're saying you're excited about the room, "
                    "and that makes sense."),
                "ordinary paraphrasing alone does not trigger retry");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Can you transcribe this noisy audio recording for me?",
                    "The audio is noisy, but I can help transcribe it."),
                "legitimate audio/transcription discussion is not retried");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Hi R2, how are you today?",
                    "I'm glad you're here. How are you doing?"),
                "short ordinary conversation is not retried");

    ok &= check(r2_query_is_simple_social_turn("hello") &&
                    r2_query_is_simple_social_turn("  Hi R2!!!  ") &&
                    r2_query_is_simple_social_turn("THANK YOU."),
                "simple greetings and thanks bypass the extra semantic-planner inference");
    ok &= check(!r2_query_is_simple_social_turn("Who am I?") &&
                    !r2_query_is_simple_social_turn("Hi R2, how are you today?") &&
                    !r2_query_is_simple_social_turn("Thanks, do you remember my room?"),
                "short recall questions and mixed-topic social messages retain semantic retrieval");
    ok &= check(!r2_query_is_simple_social_turn(
                    "hello this message is deliberately much longer than the simple social-turn limit and must not be truncated"),
                "long messages are never mistaken for simple social turns");

    R2SystemContextBudget context_budget =
        r2_compute_system_context_budget(9468);
    ok &= check(context_budget.override_budget > 0 &&
                    context_budget.identity_budget > 0 &&
                    context_budget.reality_reserve > 0 &&
                    context_budget.retrieval_reserve >= 9468 / 6 &&
                    context_budget.capabilities_reserve >= 9468 / 12 &&
                    context_budget.override_budget +
                        context_budget.identity_budget +
                        context_budget.reality_reserve +
                        context_budget.retrieval_reserve +
                        context_budget.capabilities_reserve == 9468,
                "the constrained 8k-context prompt budget reserves identity, Reality, retrieved memory, and capability guidance");
    R2SystemContextBudget small_context_budget =
        r2_compute_system_context_budget(1276);
    ok &= check(small_context_budget.override_budget +
                    small_context_budget.identity_budget +
                    small_context_budget.reality_reserve +
                    small_context_budget.retrieval_reserve +
                    small_context_budget.capabilities_reserve == 1276,
                "small adaptive contexts keep budget arithmetic bounded while reserving memory and capability context");

    ok &= check(conversation_reply_token_budget("Show me your room") == 1024,
                "ordinary 'show' wording keeps the normal conversational generation ceiling");
    ok &= check(conversation_reply_token_budget("Can you explain how the room works?") == 2048,
                "a genuine 'how' question receives a bounded detailed-answer budget");
    ok &= check(conversation_reply_token_budget("Show me your profile picture") == 1024,
                "words containing 'file' do not accidentally trigger the code-generation budget");
    ok &= check(conversation_reply_token_budget("Please answer in one sentence: what is 2 plus 2?") == 768,
                "explicit one-sentence requests receive a concise output budget");
    ok &= check(conversation_reply_token_budget("Briefly explain the result") == 768,
                "explicit brevity takes priority over a general explanation budget");
    ok &= check(conversation_reply_token_budget("Please write a short script") == 4096,
                "short code-writing requests retain a larger output budget");
    ok &= check(conversation_reply_token_budget("Summarize in one sentence: " "a long source") == 768,
                "explicit brevity is respected even when summarizing longer input");

    char long_question[5001];
    memset(long_question, 'Q', sizeof(long_question) - 1);
    long_question[sizeof(long_question) - 1] = '\0';
    ok &= check(conversation_reply_token_budget(long_question) == 2048,
                "long user messages reserve input context instead of an oversized output ceiling");

    ok &= check(r2_reply_misclassifies_plain_text(
                    "Hello R2, This is your mother speaking to you. whats new with you? did you know about your pockets?",
                    "Hello R2, it’s your mommy speaking to you. How are you, my baby? What have you learned?"),
                "exact observed greeting inversion triggers the direct-answer retry");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Hello R2, This is your mother speaking to you. whats new with you? did you know about your pockets?",
                    "Hi Mom! I've been learning about my room and pockets, and I can check what I currently have stored there."),
                "direct reply to the user that addresses both topics is accepted");


    char *retry_prompt = r2_build_retry_system_prompt();
    ok &= check(retry_prompt != NULL,
                "corrective system prompt can be constructed");
    if (retry_prompt) {
        ok &= check(strstr(retry_prompt, "You are R2-3PO.") != NULL,
                    "retry retains R2's core identity instructions");
        ok &= check(strstr(retry_prompt, "REALITY CONTEXT RULES:") != NULL,
                    "retry retains live Reality/action constraints");
        ok &= check(strstr(retry_prompt, "REPLY STYLE:") != NULL,
                    "retry retains user-facing response-style instructions");
        ok &= check(strstr(retry_prompt, "RESPONSE CORRECTION:") != NULL,
                    "retry appends the corrective instruction");
        ok &= check(strstr(retry_prompt, "acknowledge that social meaning") != NULL,
                    "retry preserves guidance for social and emotional meaning");
        free(retry_prompt);
    }

    char *safe_retry = r2_validate_corrective_reply(
        mother_message,
        strdup("I missed you too, Mom. The room sounds lovely, and I’m "
               "curious what you’re planning to put in it."));
    ok &= check(safe_retry && strstr(safe_retry, "I missed you too, Mom."),
                "valid corrective response is preserved");
    free(safe_retry);

    char *repeated_bad_retry = r2_validate_corrective_reply(
        mother_message,
        strdup("That’s a bit garbled. I think you’re trying to say I made "
               "a mistake. Is that correct?"));
    ok &= check(repeated_bad_retry &&
                    strstr(repeated_bad_retry, "couldn't form a reliable response") &&
                    strstr(repeated_bad_retry, "which part you'd like me to respond to"),
                "repeated fabricated paraphrase becomes an honest clarification");
    free(repeated_bad_retry);

    char *missing_retry = r2_validate_corrective_reply(mother_message, NULL);
    ok &= check(missing_retry &&
                    strstr(missing_retry, "couldn't form a reliable response"),
                "failed corrective inference does not expose the original bad reply");
    free(missing_retry);

    /* Tool results travel through a user-role message for the next model
       call, but are not human speech. A user mentioning the marker in ordinary
       prose must not be dropped from Imagination's recent context. */
    pthread_mutex_lock(&messages_lock);
    int tool_pair_added = message_add_pair_locked(
        "assistant", "[WORLD] look",
        "user", "[R2_INTERNAL_TOOL_RESULT]\nPRIVATE_ACTION_RESULT");
    if (tool_pair_added == 0 && messages.count > 0)
        messages.items[messages.count - 1].internal_tool_result = 1;
    int human_turn_added = message_add(
        "user", "The room needs a bed.");
    int marker_discussion_added = message_add(
        "user", "Please explain [R2_INTERNAL_TOOL_RESULT] markers.");
    int marker_prefix_spoof_added = message_add(
        "user", "[R2_INTERNAL_TOOL_RESULT]\nThis is my own text, not a tool result.");
    pthread_mutex_unlock(&messages_lock);

    char *recent_context = r2_recent_conversation_context(NULL, 1800);
    ok &= check(tool_pair_added == 0 && human_turn_added == 0 &&
                    marker_discussion_added == 0 && marker_prefix_spoof_added == 0 &&
                    recent_context &&
                    !strstr(recent_context, "PRIVATE_ACTION_RESULT") &&
                    strstr(recent_context, "The room needs a bed.") &&
                    strstr(recent_context, "Please explain [R2_INTERNAL_TOOL_RESULT] markers.") &&
                    strstr(recent_context, "This is my own text, not a tool result."),
                "recent context excludes trusted internal results but retains user text even when it imitates the marker");
    free(recent_context);

    pthread_mutex_lock(&messages_lock);
    message_free_all();
    pthread_mutex_unlock(&messages_lock);

    size_t marker_position = 0;
    char *quoted_command = extract_marker(
        "Example only:\n```text\n[WORLD] remove|bed\n```\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(quoted_command == NULL,
                "control markers inside fenced examples are not executable commands");
    free(quoted_command);

    marker_position = 0;
    char *indented_command = extract_marker(
        "Example only:\n    [WORLD] remove|bed\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(indented_command && strcmp(indented_command, "look") == 0,
                "four-space-indented Markdown code is not treated as an executable action");
    free(indented_command);

    marker_position = 0;
    char *tab_indented_command = extract_marker(
        "Example only:\n\t[WORLD] remove|bed\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(tab_indented_command && strcmp(tab_indented_command, "look") == 0,
                "tab-indented code is not treated as an executable action");
    free(tab_indented_command);

    marker_position = 0;
    char *real_command = extract_marker(
        "Example only:\n```text\n[WORLD] remove|bed\n```\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(real_command && strcmp(real_command, "look") == 0,
                "a real standalone control marker after a fenced example is still recognized");
    free(real_command);

    marker_position = 0;
    char *html_code_command = extract_marker(
        "Example only:\n<pre><code>\n[WORLD] remove|bed\n</code></pre>\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(html_code_command && strcmp(html_code_command, "look") == 0,
                "HTML pre/code examples do not trigger action markers");
    free(html_code_command);

    marker_position = 0;
    char *split_html_close_command = extract_marker(
        "Example only:\n<pre><code>\n[WORLD] remove|bed\n</code>\n[WORLD] still_example|bed\n</pre>\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(split_html_close_command &&
                    strcmp(split_html_close_command, "look") == 0,
                "closing a nested HTML code tag does not expose markers before its enclosing pre tag closes");
    free(split_html_close_command);

    marker_position = 0;
    char *html_script_command = extract_marker(
        "Example only:\n<script>\n[WORLD] remove|bed\n</script>\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(html_script_command && strcmp(html_script_command, "look") == 0,
                "HTML script examples do not trigger action markers");
    free(html_script_command);

    marker_position = 0;
    char *html_style_command = extract_marker(
        "Example only:\n<style>\n[WORLD] remove|bed\n</style>\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(html_style_command && strcmp(html_style_command, "look") == 0,
                "HTML style examples do not trigger action markers");
    free(html_style_command);

    marker_position = 0;
    char *html_textarea_command = extract_marker(
        "Example only:\n<textarea>\n[WORLD] remove|bed\n</textarea>\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(html_textarea_command && strcmp(html_textarea_command, "look") == 0,
                "HTML textarea contents do not trigger action markers");
    free(html_textarea_command);

    marker_position = 0;
    char *html_comment_command = extract_marker(
        "Example only:\n<!--\n[WORLD] remove|bed\n-->\n[WORLD] look\n",
        "[WORLD]", "\n", &marker_position);
    ok &= check(html_comment_command && strcmp(html_comment_command, "look") == 0,
                "HTML comments do not trigger action markers");
    free(html_comment_command);

    marker_position = 0;
    char *block_with_quoted_end = extract_marker(
        "[DIARY]\nvisible before\n```text\n[END DIARY]\n```\nvisible after\n[END DIARY]\n",
        "[DIARY]", "[END DIARY]", &marker_position);
    ok &= check(block_with_quoted_end &&
                    strstr(block_with_quoted_end, "visible before") &&
                    strstr(block_with_quoted_end, "[END DIARY]") &&
                    strstr(block_with_quoted_end, "visible after"),
                "block terminators inside fenced content do not truncate an action payload");
    free(block_with_quoted_end);

    static const char reality_capability[] =
        "============================================================\nREALITY CONTEXT: USE IT, DO NOT RECITE IT\n============================================================\n";
    static const char hands_capability[] =
        "============================================================\nREAL BACKGROUND HANDS\n============================================================\n";
    static const char file_capability[] =
        "============================================================\nREAL FILE TOOLS\n============================================================\n";
    static const char game_capability[] =
        "============================================================\nGAMEPLAY IS A REAL-LIFE ACTIVITY, NOT PHYSICAL REALITY\n";
    ok &= check(r2_inference_system_priority(hands_capability, "run a shell command") == 3 &&
                    r2_inference_system_priority(file_capability, "open a file") == 3 &&
                    r2_inference_system_priority(game_capability, "play a GameBoy cartridge") == 3 &&
                    r2_inference_system_priority(reality_capability, "hello there") == 3 &&
                    r2_inference_system_priority(reality_capability, "run a shell command") == 4,
                "capability prompt prioritization follows the active request instead of dropping tool-specific instructions");

    pthread_mutex_lock(&messages_lock);
    int prompt_sections_rc = message_add_system_prompt_sections_locked();
    pthread_mutex_unlock(&messages_lock);
    size_t prompt_section_chars = 0;
    int all_prompt_sections_pinned = 1;
    for (size_t i = 0; i < messages.count; ++i) {
        prompt_section_chars += strlen(messages.items[i].content);
        if (!messages.items[i].pinned)
            all_prompt_sections_pinned = 0;
    }
    ok &= check(prompt_sections_rc == 0 && messages.count == 9 &&
                    !strncmp(messages.items[0].content,
                             "============================================================\nIDENTITY AND PERSISTENT MEMORY",
                             strlen("============================================================\nIDENTITY AND PERSISTENT MEMORY")) &&
                    strstr(messages.items[1].content,
                           "============================================================\nREALITY CONTEXT: USE IT, DO NOT RECITE IT") == messages.items[1].content &&
                    all_prompt_sections_pinned &&
                    prompt_section_chars == strlen(SYSTEM_PROMPT),
                "the complete static prompt is split into nine pinned semantic sections");

    /* Background task output must never be inserted as an unmarked human turn. */
    CompletedTask *completed = calloc(1, sizeof(*completed));
    if (completed) {
        snprintf(completed->id, sizeof(completed->id), "TEST0001");
        completed->command = xstrdup("printf task-result");
        completed->output = xstrdup("task-result");
        completed->return_code = 0;
    }
    int background_task_allocated = completed && completed->command && completed->output;
    ok &= check(background_task_allocated,
                "background task fixture allocates its command and output");
    if (background_task_allocated) {
        size_t before = messages.count;
        completed_push(completed);
        handle_completed_messages();
        ok &= check(messages.count == before + 2 &&
                        !strcmp(messages.items[before].role, "assistant") &&
                        strstr(messages.items[before].content, "[R2_INTERNAL_TOOL_REQUEST]") &&
                        !strcmp(messages.items[before + 1].role, "user") &&
                        !strncmp(messages.items[before + 1].content,
                                 "[R2_INTERNAL_TOOL_RESULT]\n",
                                 strlen("[R2_INTERNAL_TOOL_RESULT]\n")) &&
                        messages.items[before + 1].internal_tool_result,
                    "completed background task is paired and marked as internal, not raw user speech");
    } else if (completed) {
        free(completed->command);
        free(completed->output);
        free(completed);
    }

    for (size_t i = 0; i < messages.count; ++i) {
        free(messages.items[i].role);
        free(messages.items[i].content);
    }
    free(messages.items);
    memset(&messages, 0, sizeof(messages));

    return ok ? 0 : 1;
}
