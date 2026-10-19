#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int r2_init(void);
void r2_shutdown(void);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s hands|diary\n", argv[0]);
        return 2;
    }

    const char *failure_env = NULL;
    if (strcmp(argv[1], "hands") == 0) {
        failure_env = "R2_TEST_FAIL_HANDS_THREAD_CREATE";
    } else if (strcmp(argv[1], "diary") == 0) {
        failure_env = "R2_TEST_FAIL_DIARY_THREAD_CREATE";
    } else {
        fprintf(stderr, "unknown startup failure scenario: %s\n", argv[1]);
        return 2;
    }

    if (setenv(failure_env, "1", 1) != 0) {
        perror("setenv");
        return 2;
    }

    if (r2_init() == 0) {
        fprintf(stderr, "r2_init unexpectedly succeeded with %s injected\n",
                failure_env);
        r2_shutdown();
        return 1;
    }

    /* This must be safe after either partial-startup failure. The test process
       exits only after the same centralized cleanup path used by main(). */
    r2_shutdown();
    return 0;
}
