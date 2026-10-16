#include "r2.h"

#include <stdio.h>

int main(void)
{
    if (r2_ollama_vision_request_should_abort()) {
        fprintf(stderr, "unexpected abort before any request is active\n");
        return 1;
    }

    if (!r2_ollama_vision_request_begin()) {
        fprintf(stderr, "first opportunistic vision request should acquire an idle gate\n");
        return 1;
    }

    if (r2_ollama_vision_request_begin()) {
        r2_ollama_vision_request_end();
        fprintf(stderr, "second vision request must not overlap the first\n");
        return 1;
    }

    r2_ollama_vision_request_end();

    if (!r2_ollama_vision_request_begin()) {
        fprintf(stderr, "vision gate should become available after release\n");
        return 1;
    }
    r2_ollama_vision_request_end();

    puts("Ollama shared request-gate smoke test passed.");
    return 0;
}
