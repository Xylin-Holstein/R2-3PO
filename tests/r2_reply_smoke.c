#include "R2Reply.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *reply = r2_reply_sanitize("Hello there. <think>I should plan what to say.</think> How are you?");
    assert(reply);
    assert(!strstr(reply, "<think>"));
    assert(!strstr(reply, "I should plan"));
    assert(strstr(reply, "Hello there.") && strstr(reply, "How are you?"));
    free(reply);

    /* Nested same-type tags must not expose text after an inner close. */
    reply = r2_reply_sanitize("<think>outer private <think>nested private</think> still private</think>Public answer.");
    assert(reply && !strcmp(reply, "Public answer."));
    free(reply);

    reply = r2_reply_sanitize("<analysis>private scratch</analysis>\n<reasoning>another private block</reasoning>\nHere is the actual answer.");
    assert(reply && !strcmp(reply, "Here is the actual answer."));
    free(reply);

    reply = r2_reply_sanitize("Analysis: I should think this through.\nFinal: The answer is 42.");
    assert(reply && !strcmp(reply, "The answer is 42."));
    free(reply);

    /* A truncated private block must not leak its contents. */
    reply = r2_reply_sanitize("Visible preface. <think>unfinished private thoughts");
    assert(reply && !strcmp(reply, "Visible preface."));
    free(reply);

    reply = r2_reply_sanitize("  \n  <think>only private content</think>  ");
    assert(reply == NULL);

    reply = r2_reply_sanitize("We need answer the user with a concise response.");
    assert(reply == NULL);
    reply = r2_reply_sanitize("We need answer the user.\nFinal: Here is the answer.");
    assert(reply && !strcmp(reply, "Here is the answer."));
    free(reply);

    /* Private tags at the beginning must not hide a scratch preamble after
       the tags have been removed. */
    reply = r2_reply_sanitize("<think>private planning</think>We need answer the user with a concise response.");
    assert(reply == NULL);
    reply = r2_reply_sanitize("<think>private planning</think>We need answer the user.\nFinal: Safe answer.");
    assert(reply && !strcmp(reply, "Safe answer."));
    free(reply);

    /* Literal private-tag syntax in Markdown code is user-facing content,
       not an instruction to remove the example. */
    reply = r2_reply_sanitize("Use `<think>example</think>` literally.");
    assert(reply && !strcmp(reply, "Use `<think>example</think>` literally."));
    free(reply);

    reply = r2_reply_sanitize(
        "Example:\n```xml\n<think>sample</think>\n```\nThis is the literal syntax.");
    assert(reply && strstr(reply, "```xml\n<think>sample</think>\n```"));
    assert(strstr(reply, "This is the literal syntax."));
    free(reply);

    reply = r2_reply_sanitize("Actual <think>private material</think> public answer.");
    assert(reply && !strstr(reply, "private material"));
    assert(strstr(reply, "public answer."));
    free(reply);

    reply = r2_reply_sanitize("A normal reply about analysis: the results are useful.");
    assert(reply && !strcmp(reply, "A normal reply about analysis: the results are useful."));
    free(reply);

    /* Crossing mixed-type tags must remain private until the outer tag closes. */
    reply = r2_reply_sanitize("<think>outer <analysis>inner </think> STILL_PRIVATE </analysis> outer_private</think>Visible answer.");
    assert(reply && !strcmp(reply, "Visible answer."));
    free(reply);

    /* An unclosed nested tag must not let a mismatched outer close expose text. */
    reply = r2_reply_sanitize("<think>outer <analysis>inner</think> still private");
    assert(reply == NULL);

    /* An unclosed private block may preserve text that was already outside it. */
    reply = r2_reply_sanitize("Safe preface. <think>unfinished private material");
    assert(reply && !strcmp(reply, "Safe preface."));
    free(reply);

    reply = r2_reply_sanitize("");
    assert(reply == NULL);

    /* A long response with one private block must not need a pointer-sized
       stack slot for every byte of input. This also guards large-input safety. */
    const size_t prefix_len = 512U * 1024U;
    const size_t suffix_len = 512U * 1024U;
    const char *private_block = "<think>private</think>";
    const size_t private_len = strlen(private_block);
    char *large_input = malloc(prefix_len + private_len + suffix_len + 1);
    assert(large_input);
    memset(large_input, 'a', prefix_len);
    memcpy(large_input + prefix_len, private_block, private_len);
    memset(large_input + prefix_len + private_len, 'b', suffix_len);
    large_input[prefix_len + private_len + suffix_len] = '\0';

    reply = r2_reply_sanitize(large_input);
    assert(reply);
    assert(strlen(reply) == prefix_len + suffix_len);
    assert(reply[prefix_len - 1] == 'a');
    assert(reply[prefix_len] == 'b');
    assert(reply[prefix_len + suffix_len - 1] == 'b');
    free(reply);
    free(large_input);

    puts("R2 user-facing reply sanitizer smoke passed");
    return 0;
}
