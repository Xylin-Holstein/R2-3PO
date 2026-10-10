#define main r2_embedded_program_main
#include "../r2.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: audio_model_payload_smoke <wav-path>\n");
        return 2;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "curl initialization failed\n");
        return 2;
    }

    pthread_mutex_lock(&messages_lock);
    int add_rc = message_add("user", "Please transcribe the current audio.");
    pthread_mutex_unlock(&messages_lock);
    if (add_rc != 0 || !latest_user_explicitly_requested_hearing()) {
        fprintf(stderr, "explicit current-turn listening intent was not recognized\n");
        curl_global_cleanup();
        return 1;
    }
    pthread_mutex_lock(&messages_lock);
    add_rc = message_add("user", "Now tell me a joke instead.");
    pthread_mutex_unlock(&messages_lock);
    if (add_rc != 0 || latest_user_explicitly_requested_hearing()) {
        fprintf(stderr, "stale listening intent incorrectly authorized capture on an unrelated turn\n");
        curl_global_cleanup();
        return 1;
    }

    /* Exercise the public model bridge without starting the robot, touching
       its databases, or requiring an installed Ollama daemon. The Python
       test supplies a local fake /api/chat endpoint and validates the payload. */
    core_initialized = 1;
    char *result = r2_model_generate_audio(
        "You are a test audio recognizer.",
        "Transcribe the supplied audio.",
        argv[1], 32);
    core_initialized = 0;
    curl_global_cleanup();

    if (!result || strcmp(result, "TRANSCRIPT: fixture audio") != 0) {
        fprintf(stderr, "unexpected audio model response: %s\n",
                result ? result : "(null)");
        free(result);
        return 1;
    }
    free(result);
    puts("Shared-model WAV audio payload smoke test passed.");
    return 0;
}
