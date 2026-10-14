#include "R2Reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *label, const char *input, const char *expected)
{
    char *actual = r2_reply_sanitize(input);
    int ok = expected ? (actual && strcmp(actual, expected) == 0) : actual == NULL;
    if (!ok) {
        fprintf(stderr, "FAIL: %s: expected <%s>, got <%s>\n",
                label, expected ? expected : "(NULL)", actual ? actual : "(NULL)");
        free(actual);
        return 1;
    }
    printf("PASS: %s\n", label);
    free(actual);
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += check("preserves ordinary text around hidden reasoning",
                      "Hello <think>private thought</think> world", "Hello  world");
    failures += check("balances nested same-tag private blocks",
                      "<think>outer <think>inner</think> still private</think>Visible reply",
                      "Visible reply");
    failures += check("removes nested mixed private blocks",
                      "<think>outer <analysis>inner</analysis> still private</think>Visible",
                      "Visible");
    failures += check("rejects an unclosed private block",
                      "Visible-looking prefix <think>private material", NULL);
    failures += check("selects labeled final section",
                      "analysis: hidden plan\nfinal: The answer is 42.", "The answer is 42.");
    failures += check("catches scratch preamble exposed by tag removal",
                      "<think>hidden</think>We need answer the user.\nfinal: A useful answer.",
                      "A useful answer.");
    failures += check("does not expose content after an unclosed outer block",
                      "<think>private <think>nested</think> still private", NULL);
    failures += check("rejects empty input", "", NULL);
    if (failures) {
        fprintf(stderr, "%d reply sanitizer regression(s) failed.\n", failures);
        return EXIT_FAILURE;
    }
    puts("R2 reply sanitizer regression suite passed.");
    return EXIT_SUCCESS;
}
