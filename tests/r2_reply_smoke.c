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

    reply = r2_reply_sanitize("A normal reply about analysis: the results are useful.");
    assert(reply && !strcmp(reply, "A normal reply about analysis: the results are useful."));
    free(reply);

    puts("R2 user-facing reply sanitizer smoke passed");
    return 0;
}
