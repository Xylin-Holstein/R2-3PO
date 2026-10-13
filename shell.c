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
#include <math.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

#include "shell.h"
#include "r2.h"
#include "Log.h"
#include "Reality.h"


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


/* The GUI talks to this local socket; all writes still pass through Reality APIs. */
static pthread_t tv_control_thread;
static int tv_control_started = 0;
static int tv_control_fd = -1;
static atomic_int tv_control_stop = ATOMIC_VAR_INIT(0);
static char tv_control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static void *tv_control_server(void *unused)
{
    (void)unused;
    /* Keep a stable descriptor value while the main thread shuts the socket down. */
    int server_fd = tv_control_fd;
    while (!atomic_load(&tv_control_stop)) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }

        char command[1200] = {0};
        size_t used = 0;
        while (used < sizeof(command) - 1) {
            ssize_t got = recv(client, command + used, sizeof(command) - 1 - used, 0);
            if (got <= 0) break;
            used += (size_t)got;
            if (memchr(command, '\n', used)) break;
        }
        command[used] = '\0';
        ssize_t got = (ssize_t)used;
        while (got > 0 && (command[got - 1] == '\n' || command[got - 1] == '\r'))
            command[--got] = '\0';

        const char *reply = "ERROR invalid command";
        char *status = NULL;
        if (got > 0 && !strcmp(command, "status")) {
            status = r2_reality_tv_status();
            reply = status ? status : "ERROR unable to read TV state";
        } else if (!strcmp(command, "display opened")) {
            reply = r2_reality_tv_display_event(1) == 0 ? "OK TV display-open event recorded" : "ERROR could not record TV display-open event";
        } else if (!strcmp(command, "display closed")) {
            reply = r2_reality_tv_display_event(0) == 0 ? "OK TV display-close event recorded" : "ERROR could not record TV display-close event";
        } else if (!strcmp(command, "power on")) {
            reply = r2_reality_tv_power(1) == 0 ? "OK powered on" : "ERROR power-on failed";
        } else if (!strcmp(command, "power off")) {
            reply = r2_reality_tv_power(0) == 0 ? "OK powered off" : "ERROR power-off failed";
        } else if (!strncmp(command, "input ", 6)) {
            char *end = NULL;
            long n = strtol(command + 6, &end, 10);
            if (end != command + 6 && *end == '\0' && n >= 1 && n <= 4)
                reply = r2_reality_tv_select_input((int)n) == 0 ? "OK input selected" : "ERROR input selection failed";
            else reply = "ERROR input must be 1..4";
        } else if (!strncmp(command, "tune ", 5)) {
            char *end = NULL;
            long n = strtol(command + 5, &end, 10);
            if (end != command + 5 && *end == '\0' && n >= 2 && n <= 13)
                reply = r2_reality_tv_tune_rf((int)n) == 0 ? "OK RF channel tuned" : "ERROR RF tuning failed";
            else reply = "ERROR RF channel must be 2..13";
        } else if (!strncmp(command, "media ", 6)) {
            char *payload = command + 6;
            char *separator = strchr(payload, '|');
            if (separator) {
                *separator = '\0';
                const char *path = separator + 1;
                const char *allowed_actions[] = {
                    "vcr_playback_started", "vcr_playback_paused",
                    "vcr_playback_stopped", "vcr_playback_ended",
                    "vcr_playback_error"
                };
                int allowed = 0;
                for (size_t i = 0; i < sizeof(allowed_actions) / sizeof(allowed_actions[0]); ++i)
                    if (!strcmp(payload, allowed_actions[i])) { allowed = 1; break; }
                if (allowed && path[0] == '/' && *path &&
                    !strchr(path, '\n') && !strchr(path, '\r') &&
                    strlen(path) < 1100) {
                    reply = r2_log_media_event(payload, "media", path,
                        "Observed from the CRT built-in VCR's VLC player; this records renderer state, not proof of what a person watched.") >= 0
                        ? "OK media event recorded" : "ERROR could not record media event";
                } else {
                    reply = "ERROR invalid media event";
                }
            } else {
                reply = "ERROR invalid media event";
            }
        } else if (!strncmp(command, "vcr insert ", 11)) {
            const char *path = command + 11;
            reply = *path && r2_reality_tv_vcr_insert(path) == 0
                ? "OK VCR tape inserted" : "ERROR unsupported or unreadable media file";
        } else if (!strcmp(command, "vcr play")) {
            reply = r2_reality_tv_vcr_transport("play") == 0 ? "OK VCR playing" : "ERROR no tape inserted";
        } else if (!strcmp(command, "vcr pause")) {
            reply = r2_reality_tv_vcr_transport("pause") == 0 ? "OK VCR paused" : "ERROR no tape inserted";
        } else if (!strcmp(command, "vcr stop")) {
            reply = r2_reality_tv_vcr_transport("stop") == 0 ? "OK VCR stopped" : "ERROR no tape inserted";
        } else if (!strcmp(command, "vcr eject")) {
            reply = r2_reality_tv_vcr_transport("eject") == 0 ? "OK VCR tape ejected" : "ERROR no tape inserted";
        } else if (!strncmp(command, "vcr position ", 13)) {
            char *end = NULL;
            double seconds = strtod(command + 13, &end);
            if (end != command + 13 && *end == '\0' && isfinite(seconds) && seconds >= 0.0)
                reply = r2_reality_tv_vcr_set_position(seconds) == 0 ? "OK VCR position saved" : "ERROR no inserted tape";
            else reply = "ERROR invalid VCR position";
        }

        size_t reply_len = strlen(reply);
        size_t reply_sent = 0;
        while (reply_sent < reply_len) {
            ssize_t sent = send(client, reply + reply_sent,
                                reply_len - reply_sent, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) break;
            reply_sent += (size_t)sent;
        }
        free(status);
        close(client);
    }
    return NULL;
}

static int tv_control_start(void)
{
    if (tv_control_started) return 0;
    const char *configured_path = getenv("R2_TV_SOCKET");
    const char *default_path = "/home/x/R2_Home/R2/tv-control.sock";
    int n = snprintf(tv_control_path, sizeof(tv_control_path), "%s",
                     configured_path && *configured_path ? configured_path : default_path);
    if (n < 0 || (size_t)n >= sizeof(tv_control_path)) return -1;

    struct stat st;
    if (lstat(tv_control_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) return -1;
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0) return -1;
        struct sockaddr_un existing;
        memset(&existing, 0, sizeof(existing));
        existing.sun_family = AF_UNIX;
        snprintf(existing.sun_path, sizeof(existing.sun_path), "%s", tv_control_path);
        int connected = connect(probe, (struct sockaddr *)&existing, sizeof(existing));
        int connect_error = errno;
        close(probe);
        if (connected == 0 || connect_error != ECONNREFUSED) return -1;
        if (unlink(tv_control_path) != 0) return -1; /* stale socket only */
    } else if (errno != ENOENT) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", tv_control_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(tv_control_path, S_IRUSR | S_IWUSR) != 0 || listen(fd, 4) != 0) {
        close(fd);
        unlink(tv_control_path);
        return -1;
    }
    tv_control_fd = fd;
    atomic_store(&tv_control_stop, 0);
    if (pthread_create(&tv_control_thread, NULL, tv_control_server, NULL) != 0) {
        close(fd);
        tv_control_fd = -1;
        unlink(tv_control_path);
        return -1;
    }
    tv_control_started = 1;
    return 0;
}

static void tv_control_stop_server(void)
{
    if (!tv_control_started) return;
    atomic_store(&tv_control_stop, 1);
    shutdown(tv_control_fd, SHUT_RDWR);
    close(tv_control_fd);
    tv_control_fd = -1;
    pthread_join(tv_control_thread, NULL);
    unlink(tv_control_path);
    tv_control_started = 0;
}



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

    if (!reply || !*reply) {
        fprintf(stderr,
                "[R2] Conversation backend returned no visible answer; the shell is printing a user-facing failure message.\n");
        printf(
            "\n"
            "R2-3PO: I'm sorry, Mom—the conversation backend didn't return a usable reply. "
            "Your message reached me, but I couldn't answer it this time. Please try again.\n"
            "\n"
        );
        free(reply);
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

static void shell_tv(const char *argument)
{
    if (!r2_reality_is_initialized()) {
        printf("[TV] Reality engine is not initialized.\n");
        return;
    }
    if (!argument || !*argument || !strcasecmp(argument, "status")) {
        char *status = r2_reality_tv_status();
        printf("%s", status ? status : "[TV] Could not read TV state.\n");
        free(status);
        return;
    }
    if (!strcasecmp(argument, "on") || !strcasecmp(argument, "power on")) {
        printf(r2_reality_tv_power(1) == 0 ? "[TV] Powered on. Eyes/attention were not changed.\n" : "[TV] Power-on failed.\n");
        return;
    }
    if (!strcasecmp(argument, "off") || !strcasecmp(argument, "power off")) {
        printf(r2_reality_tv_power(0) == 0 ? "[TV] Powered off. Eyes/attention were not changed.\n" : "[TV] Power-off failed.\n");
        return;
    }
    if (shell_starts_with(argument, "input ")) {
        char *end = NULL; long n = strtol(argument + 6, &end, 10);
        while (end && *end && isspace((unsigned char)*end)) ++end;
        if (end != argument + 6 && end && !*end && n >= 1 && n <= 4 &&
            r2_reality_tv_select_input((int)n) == 0)
            printf("[TV] Selected AV input %ld.\n", n);
        else printf("Usage: tv input 1..4\n");
        return;
    }
    if (shell_starts_with(argument, "tune ")) {
        char *end = NULL; long n = strtol(argument + 5, &end, 10);
        while (end && *end && isspace((unsigned char)*end)) ++end;
        if (end != argument + 5 && end && !*end && n >= 2 && n <= 13 &&
            r2_reality_tv_tune_rf((int)n) == 0) {
            char *status = r2_reality_tv_status();
            printf("%s", status ? status : "[TV] Tuned RF channel; status unavailable.\n");
            free(status);
        } else printf("Usage: tv tune <RF channel 2..13>\n");
        return;
    }
    if (shell_starts_with(argument, "connect ")) {
        char *copy = strdup(argument + 8);
        if (!copy) return;
        char *parts[3] = {0}; int count = 0; char *save = NULL;
        for (char *p = strtok_r(copy, "|", &save); p && count < 3; p = strtok_r(NULL, "|", &save))
            parts[count++] = shell_trim(p);
        int port = 0, valid_port = 0;
        if (count == 3) {
            char *end = NULL;
            errno = 0;
            long parsed = strtol(parts[2], &end, 10);
            if (errno != ERANGE && end != parts[2] && end && !*end &&
                parsed >= INT_MIN && parsed <= INT_MAX) {
                port = (int)parsed;
                valid_port = 1;
            }
        }
        int rc = count == 3 && valid_port
            ? r2_reality_tv_connect(parts[0], parts[1], port) : -1;
        if (rc == 0) printf("[TV] Connected %s to %s %d.\n", parts[0], parts[1], port);
        else printf("Usage: tv connect <device name> | input/RF | <port/channel> (AV inputs 2..4, RF channels 2..13).\n");
        free(copy);
        return;
    }
    if (shell_starts_with(argument, "disconnect ")) {
        const char *name = shell_trim((char *)argument + 11);
        printf(r2_reality_tv_disconnect(name) == 0 ? "[TV] Device disconnected.\n" : "[TV] No connected device with that name.\n");
        return;
    }
    if (shell_starts_with(argument, "vcr insert ")) {
        const char *path = shell_trim((char *)argument + 11);
        printf(r2_reality_tv_vcr_insert(path) == 0
            ? "[TV] Tape inserted; its saved position is retained.\n"
            : "[TV] Could not insert tape. Use a readable VLC-supported video file.\n");
        return;
    }
    if (shell_starts_with(argument, "vcr ")) {
        const char *action = shell_trim((char *)argument + 4);
        if (!strcasecmp(action, "play") || !strcasecmp(action, "pause") ||
            !strcasecmp(action, "stop") || !strcasecmp(action, "eject")) {
            printf(r2_reality_tv_vcr_transport(action) == 0
                ? "[TV] VCR transport updated.\n"
                : "[TV] No tape is inserted, or the transport command failed.\n");
        } else {
            printf("Usage: tv vcr insert <absolute media path> | tv vcr play | pause | stop | eject\n");
        }
        return;
    }
    printf("TV commands: tv status, tv on, tv off, tv input 1..4, tv tune <RF channel 2..13>, tv connect <name> | input/RF | <port>, tv disconnect <name>, tv vcr insert/play/pause/stop/eject.\n");
}


static int shell_dispatch(char *input)
{
    char *command = shell_trim(input);

    if (!command || !*command)
        return 1;

    if (!strcasecmp(command, "tv") || shell_starts_with(command, "tv ")) {
        shell_tv(shell_starts_with(command, "tv ") ? shell_trim(command + 3) : NULL);
        return 1;
    }


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

    if (tv_control_start() != 0) {
        fprintf(stderr, "[R2 Shell] WARNING: TV/VCR control socket could not start; TV GUI commands will be unavailable.\\n");
        r2_log_event(R2_LOG_ERROR, "tv_control_socket_start_failed",
                     "R2's private TV/VCR control socket could not start.",
                     "The conversation shell remains available; inspect stale socket ownership and R2_TV_SOCKET permissions.",
                     "r2_shell_run");
    }

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

    tv_control_stop_server();
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
    tv_control_stop_server();
    if (watch_running) {
        r2_watch_stop();
        watch_running = 0;
    }

    shell_running = 0;
    shell_shutdown = 1;
}
