#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

/*
 * ============================================================
 * R2-3PO SHELL
 * ============================================================
 *
 * The shell is an interface/controller for R2.
 *
 * It DOES NOT replace:
 *
 *   - R2 core
 *   - Memory
 *   - Ollama
 *   - Thinking
 *   - Diary
 *   - Eyes
 *   - Ears
 *
 * Those systems remain where they already live.
 *
 * shell.c communicates with the R2 core through r2.h.
 *
 * Watch is the one genuinely new subsystem controlled here.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "shell.h"
#include "r2.h"
#include "Log.h"
#include "SensoryJournal.h"
#include "AlternateSelf.h"


/* ============================================================
   SHELL CONFIGURATION
   ============================================================ */

#define SHELL_INPUT_MAX       4096
#define SHELL_PROMPT          "R2> "

#define WATCH_POLL_INTERVAL   2

#define VERSION_STRING        "R2-3PO Shell 1.0"


/* ============================================================
   SHELL STATE
   ============================================================ */

static volatile sig_atomic_t shell_shutdown = 0;

static int shell_running = 0;
static int watch_running = 0;


/* ============================================================
   SIGNAL HANDLING
   ============================================================ */

static void shell_signal_handler(int sig)
{
    (void)sig;

    shell_shutdown = 1;
}


/* ============================================================
   STRING HELPERS
   ============================================================ */

static char *shell_trim(char *text)
{
    if (!text)
        return NULL;

    while (
        *text &&
        isspace((unsigned char)*text)
    ) {
        text++;
    }

    char *end = text + strlen(text);

    while (
        end > text &&
        isspace((unsigned char)end[-1])
    ) {
        end--;
    }

    *end = '\0';

    return text;
}


static int shell_starts_with(
    const char *text,
    const char *prefix
)
{
    if (!text || !prefix)
        return 0;

    return strncasecmp(
        text,
        prefix,
        strlen(prefix)
    ) == 0;
}


/* ============================================================
   OUTPUT
   ============================================================ */

static void shell_banner(void)
{
    printf(
        "\n"
        "============================================================\n"
        "                     ROBOBOT3000\n"
        "                         R2-3PO\n"
        "============================================================\n"
        "%s\n"
        "\n"
        "Type 'help' for commands.\n"
        "Normal text is sent directly to R2.\n"
        "============================================================\n"
        "\n",
        VERSION_STRING
    );
}


static void shell_prompt(void)
{
    fflush(stdout);
    printf("%s", SHELL_PROMPT);
    fflush(stdout);
}


/* ============================================================
   HELP
   ============================================================ */

static void shell_help(void)
{
    printf(
        "\n"
        "R2-3PO SHELL COMMANDS\n"
        "------------------------------------------------------------\n"
        "\n"
        "  help\n"
        "      Show this help screen.\n"
        "\n"
        "  status\n"
        "      Show R2 core status.\n"
        "\n"
        "  diagnostics\n"
        "      Show detailed subsystem diagnostics.\n"
        "\n"
        "  talk <message>\n"
        "      Send a message to R2.\n"
        "\n"
        "  think\n"
        "      Trigger one autonomous thinking cycle.\n"
        "\n"
        "  thinking\n"
        "      Show autonomous-thinking status.\n"
        "\n"
        "  memory <query>\n"
        "      Search R2's persistent memory.\n"
        "\n"
        "  memory save <category> <text>\n"
        "      Save a memory through R2's memory system.\n"
        "\n"
        "  memory count\n"
        "      Show the number of stored memories.\n"
        "\n"
        "  diary\n"
        "      Trigger a diary-writing operation.\n"
        "\n"
        "  log\n"
        "      Show the latest Life Log events.\n"
        "\n"
        "  log status\n"
        "      Show Life Log counts and current session information.\n"
        "\n"
        "  log search <text>\n"
        "      Search the chronological Life Log.\n"
        "\n"
        "  journal [recent|search <text>|note <text>|status]\n"
        "      Browse timestamped sensory, thinking, and conversation events.\n"
        "  journal status\n"
        "      Show the indexed-entry count and journal availability.\n"
        "\n"
        "  alternate [list|show <id>|create <fields>|discard <id>|retain <id>|compare <id> <id>]\n"
        "      Explore isolated what-if branches without changing factual memories.\n"
        "      create fields: name|scenario|assumptions|predicted outcome|conclusion|evidence event ID\n"
        "\n"
        "  eyes\n"
        "      Show Eyes status.\n"
        "\n"
        "  eyes start\n"
        "      Start the existing Eyes subsystem.\n"
        "\n"
        "  eyes stop\n"
        "      Stop the existing Eyes subsystem.\n"
        "\n"
        "  vision\n"
        "      Show recent visual experiences.\n"
        "\n"
        "  vision status\n"
        "      Show the selected model and current vision state.\n"
        "\n"
        "  vision see [question]\n"
        "      Analyze a captured frame using R2's local vision model.\n"
        "\n"
        "  vision watch start|stop|status\n"
        "      Enable or disable periodic visual observation.\n"
        "\n"
        "  vision vlc\n"
        "      Connect Eyes to a visible VLC window.\n"
        "\n"
        "  vision file <path>\n"
        "      Open an image or video file as visual input.\n"
        "\n"
        "  vision search <text>\n"
        "      Search the visual experience library.\n"
        "\n"
        "  vision model <name>\n"
        "      Select the local Ollama vision model.\n"
        "\n"
        "  vision close\n"
        "      Close the current visual source.\n"
        "\n"
        "  ears\n"
        "      Show Ears status.\n"
        "\n"
        "  ears start\n"
        "      Start the existing Ears subsystem.\n"
        "\n"
        "  ears stop\n"
        "      Stop the existing Ears subsystem.\n"
        "\n"
        "  watch\n"
        "      Start R2 Watch mode.\n"
        "\n"
        "  watch status\n"
        "      Show Watch status.\n"
        "\n"
        "  watch stop\n"
        "      Stop Watch mode.\n"
        "\n"
        "  clear\n"
        "      Clear the terminal.\n"
        "\n"
        "  version\n"
        "      Show shell version.\n"
        "\n"
        "  shutdown\n"
        "      Cleanly shut down R2.\n"
        "\n"
        "  restart\n"
        "      Request an R2 program restart.\n"
        "\n"
        "  exit\n"
        "  quit\n"
        "      Leave the R2 shell.\n"
        "\n"
        "------------------------------------------------------------\n"
        "Any input that is not a command is treated as conversation.\n"
        "------------------------------------------------------------\n"
        "\n"
    );
}


/* ============================================================
   STATUS
   ============================================================ */

static void shell_status(void)
{
    printf(
        "\n"
        "================ R2 STATUS ================================\n"
        "\n"
    );

    printf(
        "Core initialized : %s\n",
        r2_is_initialized()
            ? "YES"
            : "NO"
    );

    printf(
        "Core shutting down: %s\n",
        r2_is_shutting_down()
            ? "YES"
            : "NO"
    );

    printf(
        "Model             : %s\n",
        r2_model_name()
            ? r2_model_name()
            : "(unknown)"
    );

    printf(
        "Thinking          : %s\n",
        r2_thinking_active()
            ? "ACTIVE"
            : "INACTIVE"
    );

    printf(
        "Think interval    : %d seconds\n",
        r2_think_interval()
    );

    printf(
        "Workers           : %d\n",
        r2_worker_count()
    );

    printf(
        "Memory count      : %ld\n",
        r2_memory_count()
    );

    printf(
        "Watch             : %s\n",
        watch_running
            ? "ACTIVE"
            : "INACTIVE"
    );

    printf(
        "\n"
        "============================================================\n"
        "\n"
    );
}


/* ============================================================
   TALK
   ============================================================ */

static void shell_talk(const char *message)
{
    if (!message || !*message) {
        printf("Usage: talk <message>\n");
        return;
    }

    char *reply = r2_talk(message);

    if (!reply) {
        fprintf(
            stderr,
            "[R2] Conversation request failed.\n"
        );
        return;
    }

    printf(
        "\n"
        "R2-3PO: %s\n"
        "\n",
        reply
    );

    free(reply);
}


/* ============================================================
   NORMAL CONVERSATION
   ============================================================ */

static void shell_conversation(const char *message)
{
    if (!message || !*message)
        return;

    shell_talk(message);
}


/* ============================================================
   THINKING
   ============================================================ */

static void shell_think(void)
{
    printf(
        "[R2] Starting one autonomous thinking cycle...\n"
    );

    int result = r2_think();

    if (result == 0) {
        printf(
            "[R2] Thinking cycle completed.\n"
        );
    }
    else {
        printf(
            "[R2] Thinking cycle returned an error.\n"
        );
    }
}


static void shell_thinking_status(void)
{
    printf(
        "\n"
        "Autonomous thinking: %s\n"
        "Interval           : %d seconds\n"
        "\n",
        r2_thinking_active()
            ? "ACTIVE"
            : "INACTIVE",
        r2_think_interval()
    );
}


/* ============================================================
   MEMORY
   ============================================================ */

static void shell_memory(const char *argument)
{
    if (!argument || !*argument) {
        printf(
            "Usage:\n"
            "  memory <query>\n"
            "  memory save <category> <text>\n"
            "  memory count\n"
        );
        return;
    }

    if (!strcasecmp(argument, "count")) {
        printf(
            "R2 persistent memories: %ld\n",
            r2_memory_count()
        );
        return;
    }

    if (shell_starts_with(argument, "save ")) {

        char *copy = strdup(argument + 5);

        if (!copy) {
            fprintf(
                stderr,
                "[R2] Memory allocation failed.\n"
            );
            return;
        }

        char *category = shell_trim(copy);

        char *space = strchr(category, ' ');

        if (!space) {
            printf(
                "Usage: memory save <category> <text>\n"
            );

            free(copy);
            return;
        }

        *space = '\0';

        char *memory = shell_trim(space + 1);

        if (!*memory) {
            printf(
                "Memory text cannot be empty.\n"
            );

            free(copy);
            return;
        }

        int result =
            r2_save_memory(
                memory,
                category
            );

        if (result == 0) {
            printf(
                "[R2] Memory saved.\n"
            );
        }
        else {
            printf(
                "[R2] Failed to save memory.\n"
            );
        }

        free(copy);
        return;
    }

    char *memories =
        r2_retrieve_memories(argument);

    if (!memories) {
        printf(
            "[R2] No memory results returned.\n"
        );
        return;
    }

    printf(
        "\n"
        "================ MEMORY SEARCH ============================\n"
        "%s\n"
        "============================================================\n"
        "\n",
        memories
    );

    free(memories);
}


/* ============================================================
   DIARY
   ============================================================ */

static void shell_diary(void)
{
    if (!r2_diary_active()) {
        printf(
            "[R2] Diary subsystem is not currently available.\n"
        );
        return;
    }

    printf(
        "[R2] Requesting diary operation...\n"
    );

    if (r2_write_diary() == 0) {
        printf(
            "[R2] Diary operation completed.\n"
        );
    }
    else {
        printf(
            "[R2] Diary operation failed.\n"
        );
    }
}



/* ============================================================
   LIFE LOG
   ============================================================ */

static void shell_log(const char *argument)
{
    char *result = NULL;

    if (!r2_log_is_initialized()) {
        printf("[R2 Life Log is not initialized.]\n");
        return;
    }

    if (!argument || !*argument) {
        result = r2_log_recent(25);
    } else if (!strcasecmp(argument, "status")) {
        result = r2_log_status_report();
    } else if (shell_starts_with(argument, "search ")) {
        const char *query = shell_trim((char *)argument + 7);
        result = r2_log_search(query, 50);
    } else {
        result = r2_log_search(argument, 50);
    }

    if (result) {
        printf("%s", result);
        free(result);
    } else {
        printf("[Could not read the Life Log.]\n");
    }
}

/* ============================================================
   SENSORY JOURNAL
   ============================================================ */

static void shell_journal(const char *argument)
{
    char *result = NULL;
    if (!argument || !*argument || !strcasecmp(argument, "recent")) {
        result = r2_sj_recent(25);
    } else if (shell_starts_with(argument, "search ")) {
        result = r2_sj_search(shell_trim((char *)argument + 7), 50);
    } else if (shell_starts_with(argument, "note ")) {
        const char *note = shell_trim((char *)argument + 5);
        int64_t id = r2_sj_note(note, "Manual note entered through the R2 shell.");
        if (id > 0) printf("[Sensory Journal note recorded as Life Log event %lld.]\n",
                           (long long)id);
        else printf("[Could not record the journal note. Is the Life Log initialized?]\n");
        return;
    } else if (!strcasecmp(argument, "status")) {
        long count = r2_sj_count();
        if (count >= 0)
            printf("[Sensory Journal: READY; %ld indexed entries.]\n", count);
        else
            printf("[Sensory Journal: UNAVAILABLE.]\n");
        return;
    } else {
        result = r2_sj_search(argument, 50);
    }
    if (result) { printf("%s", result); free(result); }
    else printf("[Could not read the Sensory Journal.]\n");
}

/* ============================================================
   ALTERNATE-SELF LAB
   ============================================================ */

static long long shell_parse_id(const char *text)
{
    char *end = NULL;
    if (!text || !*text) return -1;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || end == text || *shell_trim(end) != '\0' || value <= 0)
        return -1;
    return value;
}

static void shell_alternate(const char *argument)
{
    if (!argument || !*argument || !strcasecmp(argument, "list")) {
        char *result = r2_altself_list(25);
        if (result) { printf("%s", result); free(result); }
        else printf("[Alternate-Self Lab is unavailable.]\n");
        return;
    }
    if (shell_starts_with(argument, "show ")) {
        long long id = shell_parse_id(shell_trim((char *)argument + 5));
        char *result = id > 0 ? r2_altself_show(id) : NULL;
        if (result) { printf("%s", result); free(result); }
        else printf("[Usage: alternate show <branch-id>]\n");
        return;
    }
    if (shell_starts_with(argument, "discard ")) {
        long long id = shell_parse_id(shell_trim((char *)argument + 8));
        if (id > 0 && r2_altself_discard(id) == 0)
            printf("[Branch #%lld marked discarded; its history remains preserved.]\n", id);
        else printf("[Could not discard branch. Use 'alternate list' to check its ID and status.]\n");
        return;
    }
    if (shell_starts_with(argument, "retain ")) {
        long long id = shell_parse_id(shell_trim((char *)argument + 7));
        if (id > 0 && r2_altself_retain_hypothesis(id) == 0)
            printf("[Branch #%lld retained as a hypothesis, not a factual memory.]\n", id);
        else printf("[Could not retain branch. Use 'alternate list' to check its ID and status.]\n");
        return;
    }
    if (shell_starts_with(argument, "compare ")) {
        long long first = -1, second = -1;
        char extra = '\0';
        if (sscanf(argument + 8, "%lld %lld %c", &first, &second, &extra) == 2 &&
            first > 0 && second > 0) {
            char *result = r2_altself_compare(first, second);
            if (result) { printf("%s", result); free(result); }
            else printf("[Could not compare those branches.]\n");
        } else printf("[Usage: alternate compare <branch-id> <branch-id>]\n");
        return;
    }
    if (shell_starts_with(argument, "create ")) {
        char *fields_text = strdup(argument + 7);
        if (!fields_text) { printf("[Out of memory.]\n"); return; }
        char *fields[6] = {0};
        size_t count = 0;
        char *cursor = fields_text;
        while (count < 6) {
            fields[count++] = cursor;
            char *separator = strchr(cursor, '|');
            if (!separator) break;
            *separator = '\0';
            cursor = separator + 1;
        }
        if (count < 5 || (count == 6 && strchr(fields[5], '|'))) {
            printf("[Usage: alternate create name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID]\n");
            free(fields_text);
            return;
        }
        for (size_t i = 0; i < count; ++i) fields[i] = shell_trim(fields[i]);
        long long evidence = count >= 6 && *fields[5] ? shell_parse_id(fields[5]) : 0;
        if (count >= 6 && *fields[5] && evidence < 0) {
            printf("[Evidence event ID must be a positive integer, or left blank.]\n");
            free(fields_text);
            return;
        }
        int64_t id = r2_altself_create(fields[0], fields[1], fields[2],
                                       fields[3], fields[4], evidence > 0 ? evidence : 0);
        if (id > 0)
            printf("[Created hypothetical branch #%lld. It is not a real event or factual memory.]\n",
                   (long long)id);
        else
            printf("[Could not create branch. Check that name and scenario are non-empty and the lab is initialized.]\n");
        free(fields_text);
        return;
    }
    printf("Usage: alternate [list|show <id>|create name|scenario|assumptions|outcome|conclusion|evidence-id|discard <id>|retain <id>|compare <id> <id>]\n");
}

/* ============================================================
   EYES
   ============================================================ */

static void shell_eyes(const char *argument)
{
    if (!argument || !*argument) {

        int state =
            r2_eyes_status();

        printf(
            "Eyes: %s\n",
            state > 0
                ? "ACTIVE"
                : state == 0
                    ? "INACTIVE"
                    : "UNAVAILABLE"
        );

        return;
    }

    if (!strcasecmp(argument, "start")) {

        int result =
            r2_eyes_start();

        if (result == 0)
            printf("[Eyes] Started.\n");
        else
            printf("[Eyes] Failed to start.\n");

        return;
    }

    if (!strcasecmp(argument, "stop")) {

        int result =
            r2_eyes_stop();

        if (result == 0)
            printf("[Eyes] Stopped.\n");
        else
            printf("[Eyes] Failed to stop.\n");

        return;
    }

    if (!strcasecmp(argument, "status")) {

        int state =
            r2_eyes_status();

        printf(
            "Eyes: %s\n",
            state > 0
                ? "ACTIVE"
                : state == 0
                    ? "INACTIVE"
                    : "UNAVAILABLE"
        );

        return;
    }

    printf(
        "Usage: eyes [start|stop|status]\n"
    );
}



/* ============================================================
   VISUAL EXPERIENCE LIBRARY
   ============================================================ */

static void shell_vision(const char *argument)
{
    if (!r2_vision_available()) {
        printf("[Visual Experience Library is unavailable. Check R2 startup logs.]\n");
        return;
    }

    if (!argument || !*argument || !strcasecmp(argument, "recent")) {
        char *result = r2_vision_recent(10);
        if (result) { printf("%s", result); free(result); }
        else printf("[Could not read visual experiences.]\n");
        return;
    }

    if (!strcasecmp(argument, "status")) {
        printf("Vision library: %s\n", r2_vision_available() ? "READY" : "UNAVAILABLE");
        printf("Vision model: %s\n", r2_vision_model_name());
        printf("Eyes input: %s\n", r2_eyes_status() > 0 ? "OPEN" : "CLOSED");
        printf("Continuous observation: %s\n",
               r2_vision_watch_active() ? "ACTIVE" : "STOPPED");
        return;
    }

    if (!strcasecmp(argument, "see")) {
        char *result = r2_vision_see(
            "Describe what is visible in the current frame. Identify objects, visible text, "
            "layout, and relevant details. Separate direct observations from uncertain inference.");
        if (result) { printf("R2 sees:\n%s\n", result); free(result); }
        else printf("[Visual analysis failed. Check that the vision model is installed and Eyes can capture a frame.]\n");
        return;
    }

    if (shell_starts_with(argument, "see ")) {
        const char *question = shell_trim((char *)argument + 4);
        char *result = r2_vision_see(question);
        if (result) { printf("R2 sees:\n%s\n", result); free(result); }
        else printf("[Visual analysis failed. Check that the vision model is installed and Eyes can capture a frame.]\n");
        return;
    }

    if (!strcasecmp(argument, "watch start")) {
        if (r2_vision_watch_start() == 0)
            printf("[Visual observation is running; R2 will analyze a frame about every 15 seconds.]\n");
        else
            printf("[Could not start visual observation. Check the vision model and R2 status.]\n");
        return;
    }

    if (!strcasecmp(argument, "watch stop")) {
        r2_vision_watch_stop();
        printf("[Visual observation stopped.]\n");
        return;
    }

    if (!strcasecmp(argument, "watch status")) {
        printf("Visual observation: %s\n",
               r2_vision_watch_active() ? "ACTIVE" : "STOPPED");
        return;
    }

    if (!strcasecmp(argument, "vlc")) {
        if (r2_vision_open_vlc() == 0)
            printf("[Eyes connected to the visible VLC window. Use 'vision see' or 'vision watch start' after it opens.]\n");
        else
            printf("[Could not open VLC capture. Ensure VLC is visible in the graphical desktop session.]\n");
        return;
    }

    if (shell_starts_with(argument, "file ")) {
        const char *path = shell_trim((char *)argument + 5);
        if (r2_vision_open_file(path) == 0)
            printf("[Eyes opened visual file: %s]\n", path);
        else
            printf("[Could not open visual file: %s]\n", path);
        return;
    }

    if (!strcasecmp(argument, "close")) {
        if (r2_vision_close() == 0) printf("[Visual input closed.]\n");
        else printf("[No visual input could be closed.]\n");
        return;
    }

    if (shell_starts_with(argument, "search ")) {
        const char *query = shell_trim((char *)argument + 7);
        char *result = r2_vision_search(query, 25);
        if (result) { printf("%s", result); free(result); }
        else printf("[Could not search visual experiences.]\n");
        return;
    }

    if (shell_starts_with(argument, "model ")) {
        const char *model = shell_trim((char *)argument + 6);
        if (r2_vision_set_model(model) == 0)
            printf("[Vision model set to %s. Make sure this model is installed in Ollama.]\n", model);
        else
            printf("[Invalid vision model name.]\n");
        return;
    }

    printf("Usage: vision [recent|see [question]|search <text>|model <ollama-model>|vlc|file <path>|close]\n");
}

/* ============================================================
   EARS
   ============================================================ */

static void shell_ears(const char *argument)
{
    if (!argument || !*argument) {

        int state =
            r2_ears_status();

        printf(
            "Ears: %s\n",
            state > 0
                ? "ACTIVE"
                : state == 0
                    ? "INACTIVE"
                    : "UNAVAILABLE"
        );

        return;
    }

    if (!strcasecmp(argument, "start")) {

        int result =
            r2_ears_start();

        if (result == 0)
            printf("[Ears] Started.\n");
        else
            printf("[Ears] Failed to start.\n");

        return;
    }

    if (!strcasecmp(argument, "stop")) {

        int result =
            r2_ears_stop();

        if (result == 0)
            printf("[Ears] Stopped.\n");
        else
            printf("[Ears] Failed to stop.\n");

        return;
    }

    if (!strcasecmp(argument, "status")) {

        int state =
            r2_ears_status();

        printf(
            "Ears: %s\n",
            state > 0
                ? "ACTIVE"
                : state == 0
                    ? "INACTIVE"
                    : "UNAVAILABLE"
        );

        return;
    }

    printf(
        "Usage: ears [start|stop|status]\n"
    );
}


/* ============================================================
   WATCH
   ============================================================
 *
 * Watch is deliberately NOT implemented by duplicating Eyes
 * or Ears here.
 *
 * This controller tells the Watch subsystem when to operate.
 *
 * The actual capture/detection implementation can subsequently
 * be placed behind these R2 Watch bridge functions.
 */

static void shell_watch_status(void)
{
    printf(
        "\n"
        "================ WATCH STATUS =============================\n"
        "\n"
        "Watch controller : %s\n"
        "R2 Watch state   : %s\n"
        "\n"
        "Watch is intended to coordinate:\n"
        "  - microphone input\n"
        "  - desktop/system audio\n"
        "  - VLC detection\n"
        "  - VLC/video observation\n"
        "\n"
        "============================================================\n"
        "\n",
        watch_running
            ? "ACTIVE"
            : "INACTIVE",
        r2_watch_active()
            ? "ACTIVE"
            : "INACTIVE"
    );
}


static void shell_watch_start(void)
{
    if (watch_running) {
        printf(
            "[Watch] Already running.\n"
        );
        return;
    }

    printf(
        "[Watch] Starting...\n"
    );

    int result =
        r2_watch_start();

    if (result != 0) {
        printf(
            "[Watch] Failed to start.\n"
        );
        return;
    }

    watch_running = 1;

    printf(
        "[Watch] ACTIVE.\n"
    );
}


static void shell_watch_stop(void)
{
    if (!watch_running) {
        printf(
            "[Watch] Already stopped.\n"
        );
        return;
    }

    printf(
        "[Watch] Stopping...\n"
    );

    int result =
        r2_watch_stop();

    if (result != 0) {
        printf(
            "[Watch] Failed to stop.\n"
        );
        return;
    }

    watch_running = 0;

    printf(
        "[Watch] STOPPED.\n"
    );
}


static void shell_watch(const char *argument)
{
    if (!argument || !*argument) {
        shell_watch_start();
        return;
    }

    if (!strcasecmp(argument, "start")) {
        shell_watch_start();
        return;
    }

    if (!strcasecmp(argument, "stop")) {
        shell_watch_stop();
        return;
    }

    if (!strcasecmp(argument, "status")) {
        shell_watch_status();
        return;
    }

    printf(
        "Usage: watch [start|stop|status]\n"
    );
}


/* ============================================================
   TERMINAL COMMANDS
   ============================================================ */

static void shell_clear(void)
{
    printf("\033[2J\033[H");
    fflush(stdout);
}


static void shell_version(void)
{
    printf(
        "%s\n",
        VERSION_STRING
    );
}


/* ============================================================
   SHUTDOWN
   ============================================================ */

static void shell_shutdown_r2(void)
{
    printf(
        "\n"
        "[R2] Shutdown requested.\n"
    );

    watch_running = 0;
    shell_running = 0;

    r2_request_shutdown();
}


static void shell_restart(void)
{
    printf(
        "\n"
        "[R2] Restart requested.\n"
    );

    watch_running = 0;
    shell_running = 0;

    if (r2_request_restart() != 0) {
        printf(
            "[R2] Restart request failed.\n"
        );

        shell_running = 1;
    }
}


/* ============================================================
   COMMAND DISPATCH
   ============================================================ */

static int shell_dispatch(char *input)
{
    char *command =
        shell_trim(input);

    if (!command || !*command)
        return 1;


    /* --------------------------------------------------------
       HELP
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "help") ||
        !strcmp(command, "?")
    ) {
        shell_help();
        return 1;
    }


    /* --------------------------------------------------------
       STATUS
       -------------------------------------------------------- */

    if (!strcasecmp(command, "status")) {
        shell_status();
        return 1;
    }


    /* --------------------------------------------------------
       DIAGNOSTICS
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "diagnostics") ||
        !strcasecmp(command, "debug")
    ) {
        r2_diagnostics();
        return 1;
    }


    /* --------------------------------------------------------
       VERSION
       -------------------------------------------------------- */

    if (!strcasecmp(command, "version")) {
        shell_version();
        return 1;
    }


    /* --------------------------------------------------------
       THINK
       -------------------------------------------------------- */

    if (!strcasecmp(command, "think")) {
        shell_think();
        return 1;
    }


    /* --------------------------------------------------------
       THINKING STATUS
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "thinking") ||
        !strcasecmp(command, "thinking status")
    ) {
        shell_thinking_status();
        return 1;
    }


    /* --------------------------------------------------------
       MEMORY
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "memory")
    ) {
        shell_memory(NULL);
        return 1;
    }

    if (shell_starts_with(command, "memory ")) {
        shell_memory(
            shell_trim(command + 7)
        );
        return 1;
    }


    /* --------------------------------------------------------
       LIFE LOG
       -------------------------------------------------------- */

    if (!strcasecmp(command, "log")) {
        shell_log(NULL);
        return 1;
    }

    if (shell_starts_with(command, "log ")) {
        shell_log(shell_trim(command + 4));
        return 1;
    }

    /* --------------------------------------------------------
       SENSORY JOURNAL / ALTERNATE-SELF LAB
       -------------------------------------------------------- */
    if (!strcasecmp(command, "journal")) {
        shell_journal(NULL);
        return 1;
    }
    if (shell_starts_with(command, "journal ")) {
        shell_journal(shell_trim(command + 8));
        return 1;
    }
    if (!strcasecmp(command, "alternate")) {
        shell_alternate(NULL);
        return 1;
    }
    if (shell_starts_with(command, "alternate ")) {
        shell_alternate(shell_trim(command + 10));
        return 1;
    }

    /* --------------------------------------------------------
       DIARY
       -------------------------------------------------------- */

    if (!strcasecmp(command, "diary")) {
        shell_diary();
        return 1;
    }


    /* --------------------------------------------------------
       EYES
       -------------------------------------------------------- */

    if (!strcasecmp(command, "eyes")) {
        shell_eyes(NULL);
        return 1;
    }

    if (shell_starts_with(command, "eyes ")) {
        shell_eyes(
            shell_trim(command + 5)
        );
        return 1;
    }


    /* --------------------------------------------------------
       VISUAL EXPERIENCE LIBRARY
       -------------------------------------------------------- */

    if (!strcasecmp(command, "vision")) {
        shell_vision(NULL);
        return 1;
    }

    if (shell_starts_with(command, "vision ")) {
        shell_vision(shell_trim(command + 7));
        return 1;
    }


    /* --------------------------------------------------------
       EARS
       -------------------------------------------------------- */

    if (!strcasecmp(command, "ears")) {
        shell_ears(NULL);
        return 1;
    }

    if (shell_starts_with(command, "ears ")) {
        shell_ears(
            shell_trim(command + 5)
        );
        return 1;
    }


    /* --------------------------------------------------------
       WATCH
       -------------------------------------------------------- */

    if (!strcasecmp(command, "watch")) {
        shell_watch(NULL);
        return 1;
    }

    if (shell_starts_with(command, "watch ")) {
        shell_watch(
            shell_trim(command + 6)
        );
        return 1;
    }


    /* --------------------------------------------------------
       TALK
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "talk")
    ) {
        printf(
            "Usage: talk <message>\n"
        );
        return 1;
    }

    if (shell_starts_with(command, "talk ")) {

        shell_talk(
            shell_trim(command + 5)
        );

        return 1;
    }


    /* --------------------------------------------------------
       CLEAR
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "clear") ||
        !strcasecmp(command, "cls")
    ) {
        shell_clear();
        return 1;
    }


    /* --------------------------------------------------------
       SHUTDOWN
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "shutdown") ||
        !strcasecmp(command, "poweroff")
    ) {
        shell_shutdown_r2();
        return 1;
    }


    /* --------------------------------------------------------
       RESTART
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "restart") ||
        !strcasecmp(command, "reboot")
    ) {
        shell_restart();
        return 1;
    }


    /* --------------------------------------------------------
       EXIT
       -------------------------------------------------------- */

    if (
        !strcasecmp(command, "exit") ||
        !strcasecmp(command, "quit")
    ) {
        printf(
            "[R2 Shell] Leaving shell.\n"
        );

        shell_running = 0;

        return 1;
    }


    /*
     * Not a shell command.
     *
     * Treat it as ordinary conversation with R2.
     */
    shell_conversation(command);

    return 1;
}


/* ============================================================
   SHELL LOOP
   ============================================================ */

int r2_shell_run(void)
{
    char *line = NULL;
    size_t capacity = 0;

    shell_shutdown = 0;
    shell_running = 1;

    signal(
        SIGINT,
        shell_signal_handler
    );

    signal(
        SIGTERM,
        shell_signal_handler
    );

    shell_banner();

    while (
        shell_running &&
        !shell_shutdown &&
        !r2_is_shutting_down()
    ) {

        shell_prompt();

        errno = 0;

        ssize_t length =
            getline(
                &line,
                &capacity,
                stdin
            );

        if (length < 0) {

            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }

            /*
             * EOF (Ctrl-D).
             */
            printf(
                "\n[R2 Shell] End of input.\n"
            );

            break;
        }

        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';

        char *input =
            shell_trim(line);

        if (!input || !*input)
            continue;

        shell_dispatch(input);
    }

    /*
     * Watch must not remain active after the shell exits.
     */
    if (watch_running) {
        r2_watch_stop();
        watch_running = 0;
    }

    free(line);

    return 0;
}


/* ============================================================
   SHELL STATE ACCESS
   ============================================================ */

int r2_shell_is_running(void)
{
    return shell_running;
}


int r2_shell_watch_is_running(void)
{
    return watch_running;
}


/* ============================================================
   SHELL SHUTDOWN
   ============================================================ */

void r2_shell_shutdown(void)
{
    if (watch_running) {
        r2_watch_stop();
        watch_running = 0;
    }

    shell_running = 0;
    shell_shutdown = 1;
}
