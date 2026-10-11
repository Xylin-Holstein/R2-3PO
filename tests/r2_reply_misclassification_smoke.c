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
    const char *mother_message =
        "Hello r2, this is your mother speaking to you. i miss you. "
        "did you know that you had a room with stuff in it? im actually "
        "working on giving you a fully detailed room with a bed, you also "
        "have physical pockets you can put stuff in";
    const char *observed_bad_reply =
        "That’s a bit garbled, but I think you’re trying to say, "
        "“Hello, I’m R2, and I’m making a mistake.” Is that correct?";

    ok &= check(r2_reply_misclassifies_plain_text(mother_message,
                    observed_bad_reply),
                "observed fabricated paraphrase triggers corrective retry");

    ok &= check(r2_reply_misclassifies_plain_text(mother_message,
                    "I think you're trying to say that you need help."),
                "straight-apostrophe paraphrase trigger works");

    ok &= check(!r2_reply_misclassifies_plain_text(mother_message,
                    "I missed you too, Mom. A room with a bed and pockets "
                    "sounds exciting. What are you planning to put in the room?"),
                "relevant warm response does not trigger retry");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Can you transcribe this noisy audio recording for me?",
                    "The audio is noisy, but I can help transcribe it."),
                "legitimate audio/transcription discussion is not retried");

    ok &= check(!r2_reply_misclassifies_plain_text(
                    "Hi R2, how are you today?",
                    "I'm glad you're here. How are you doing?"),
                "short ordinary conversation is not retried");

    return ok ? 0 : 1;
}
