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
                    r2_keyword_is_low_signal("speaking"),
                "Life Log retrieval ignores low-signal greeting/address tokens");
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

    ok &= check(conversation_reply_token_budget("Show me your room") == 4096,
                "ordinary 'show' wording does not accidentally match the 'how' detail trigger");
    ok &= check(conversation_reply_token_budget("Can you explain how the room works?") == 6144,
                "a genuine 'how' question retains the detailed-answer budget");
    ok &= check(conversation_reply_token_budget("Show me your profile picture") == 4096,
                "words containing 'file' do not accidentally trigger the code-generation budget");
    ok &= check(conversation_reply_token_budget("Please answer in one sentence: what is 2 plus 2?") == 768,
                "explicit one-sentence requests receive a concise output budget");
    ok &= check(conversation_reply_token_budget("Briefly explain the result") == 768,
                "explicit brevity takes priority over a general explanation budget");
    ok &= check(conversation_reply_token_budget("Please write a short script") == 8192,
                "code-writing requests retain sufficient output budget even when described as short");
    ok &= check(conversation_reply_token_budget("Summarize in one sentence: " "a long source") == 768,
                "explicit brevity is respected even when summarizing longer input");

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
    int tool_result_added = message_add(
        "user", "[R2_INTERNAL_TOOL_RESULT]\nPRIVATE_ACTION_RESULT");
    int human_turn_added = message_add(
        "user", "The room needs a bed.");
    int marker_discussion_added = message_add(
        "user", "Please explain [R2_INTERNAL_TOOL_RESULT] markers.");
    pthread_mutex_unlock(&messages_lock);

    char *recent_context = r2_recent_conversation_context(NULL, 1800);
    ok &= check(tool_result_added == 0 && human_turn_added == 0 &&
                    marker_discussion_added == 0 &&
                    recent_context &&
                    !strstr(recent_context, "PRIVATE_ACTION_RESULT") &&
                    strstr(recent_context, "The room needs a bed.") &&
                    strstr(recent_context, "Please explain [R2_INTERNAL_TOOL_RESULT] markers."),
                "recent context excludes internal results but preserves human discussion of the marker");
    free(recent_context);

    pthread_mutex_lock(&messages_lock);
    message_free_all();
    pthread_mutex_unlock(&messages_lock);

    return ok ? 0 : 1;
}
