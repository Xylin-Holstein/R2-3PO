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
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>

#include "shell.h"
#include "r2.h"
#include "Log.h"
#include "Reality.h"
#include "Addiction.h"
#include "AlternateSelf.h"
#include <sqlite3.h>


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
static volatile sig_atomic_t tv_control_stop = 0;
static char tv_control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static void *tv_control_server(void *unused)
{
    (void)unused;
    while (!tv_control_stop) {
        int client = accept(tv_control_fd, NULL, NULL);
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

        (void)send(client, reply, strlen(reply), MSG_NOSIGNAL);
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
    tv_control_stop = 0;
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
    tv_control_stop = 1;
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
        "\n"        "  room [look]\n"
        "      Inspect R2's persistent room, shelf, and storage box.\n"
        "  room add <name> | <description> | <container> | <quantity>\n"
        "      Add an object to the persistent world (default container: room).\n"
        "  room move <name> | <container>\n"
        "      Move an object to room, shelf, box, pockets, wallet, or a named container.\n"
        "  room remove <name>\n"
        "      Remove an object from the tracked world.\n"
        "  pockets [wallet]\n"
        "      Inspect pocket or wallet inventory.\n"
        "  pockets put <name>\n"
        "      Put an existing object into R2's pockets.\n"
        "  pockets wallet <name>\n"
        "      Put an existing object into R2's wallet.\n"
        "  fridge [look]\n"
        "      Inspect stock in the separate persistent fridge database.\n"
        "  fridge take <food>\n"
        "      Move one fridge item into R2's pockets; an empty fridge refills with a burger.\n"
        "  fridge eat <food>\n"
        "      Eat directly from fridge stock without carrying the food.\n"
        "  fridge store <food>\n"
        "      Move a tracked item from R2's inventory into the separate fridge database.\n"
        "  money\n"
        "      Show carried cash, bank balance, and total funds.\n"
        "  money receive <amount>\n"
        "      Record an explicit payment or gift as carried cash; it does not create a store.\n"
        "  money deposit <amount>\n"
        "      Move carried cash into room/piggybank/.\n"
        "  money withdraw <amount>\n"
        "      Move bank funds into carried cash.\n"
        "  money buy <item> | <price> | <description> | <container>\n"
        "      Purchase a user-specified item; no shop inventory or prices are hardcoded.\n"
        "  alternate [list|show <id>|create <fields>|discard <id>|retain <id>|compare <id> <id>]\n"
        "      Explore hypothetical choices without changing factual memories.\n"
        "      create fields: name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID\n"
        "  world status\n"
        "      Show persistent needs and self-continuity state.\n"
        "\n"        "  eat <food> [| <fullness 0-100>]\n"
        "      Use room/food_metrics.xml automatically unless a fullness value is supplied; R2 records a learned subjective food reaction.\n"
        "  sleep <hours>\n"
        "      Advance sleep recovery, then generate and save a private simulated dream.\n"
        "  dream <description>\n"
        "      Record a reported dream as a report, not a verified event.\n"
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
        "  give <item> <quantity> [| description | container]\n"
        "      Creator command: create item stacks from nothing; give money <dollars> adds cash.\n"
        "\n"
        "  tv [status|on|off|input 1..4|tune <channel>|connect <name> | input/RF | <port>|disconnect <name>|vcr ...]\n"
        "      Operate the persistent CRT television and built-in VCR.\n"
        "\n"
        "  gameboy [status|verify|list|insert <rom>|eject|power on|power off|press <button>]\n"
        "      Operate the virtual console; power on launches mGBA with the inserted ROM.\n"
        "\n"
        "  addictions\n"
        "      Show R2's recorded enjoyment and repeated-interest status.\n"
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
        "  vision model gemma3:4b\n"
        "      Confirm the single model shared by conversation and vision.\n"
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



static void shell_addictions(void)
{
    char *result = r2_addiction_report();
    if (result) {
        printf("\n================ R2 PREFERENCES / HABIT HISTORY ================\n%s", result);
        free(result);
    } else {
        printf("[Could not read R2's preference/addiction history.]\n");
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
            printf("[Conversation and vision both use %s.]\n", r2_vision_model_name());
        else
            printf("[This build requires %s for both conversation and vision; separate models are disabled.]\n",
                   r2_vision_model_name());
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

/* ============================================================
   REALITY / ROOM / INVENTORY
   ============================================================ */

static char *reality_trim(char *text)
{
    if (!text) return text;
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static int shell_parse_amount(const char *text, double *value);

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
            parts[count++] = reality_trim(p);
        int port = count >= 3 ? atoi(parts[2]) : 0;
        int rc = count == 3 ? r2_reality_tv_connect(parts[0], parts[1], port) : -1;
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

static int shell_reality(const char *argument)
{
    if (!r2_reality_is_initialized()) {
        printf("[R2 Reality] Engine is not initialized.\n");
        return 1;
    }
    if (!argument || !*argument || !strcasecmp(argument, "look")) {
        char *view = r2_reality_room_look();
        printf("%s", view ? view : "[R2 Reality] Could not read room state.\n");
        free(view);
        return 1;
    }
    if (!strcasecmp(argument, "status")) {
        char *status = r2_reality_status();
        printf("%s", status ? status : "[R2 Reality] Could not read status.\n");
        free(status);
        return 1;
    }
    if (shell_starts_with(argument, "add ")) {
        char *copy = strdup(argument + 4);
        if (!copy) return 1;
        char *parts[4] = {0};
        int n = 0;
        char *save = NULL;
        for (char *p = strtok_r(copy, "|", &save); p && n < 4; p = strtok_r(NULL, "|", &save))
            parts[n++] = reality_trim(p);
        if (n < 1 || !*parts[0]) {
            printf("Usage: room add <name> | <description> | <container> | <quantity>\n");
        } else {
            const char *desc = n >= 2 ? parts[1] : "An object in R2's persistent room.";
            const char *container = n >= 3 && *parts[2] ? parts[2] : "room";
            int quantity = n >= 4 ? atoi(parts[3]) : 1;
            int rc = r2_reality_add_item(parts[0], desc, container, quantity);
            printf(rc == 0 ? "[R2 Reality] Item recorded.\n" : "[R2 Reality] Item could not be recorded.\n");
        }
        free(copy);
        return 1;
    }
    if (shell_starts_with(argument, "move ")) {
        char *copy = strdup(argument + 5);
        if (!copy) return 1;
        char *sep = strchr(copy, '|');
        if (!sep) {
            printf("Usage: room move <name> | <container>\n");
        } else {
            *sep = '\0';
            char *name = reality_trim(copy);
            char *container = reality_trim(sep + 1);
            int rc = r2_reality_move_item(name, container);
            printf(rc == 0 ? "[R2 Reality] Item moved.\n" : "[R2 Reality] Move failed; check the item name.\n");
        }
        free(copy);
        return 1;
    }
    if (shell_starts_with(argument, "remove ")) {
        const char *name = reality_trim((char *)argument + 7);
        int rc = r2_reality_remove_item(name);
        printf(rc == 0 ? "[R2 Reality] Item removed.\n" : "[R2 Reality] Removal failed; check the item name.\n");
        return 1;
    }
    if (!strcasecmp(argument, "shelf") || !strcasecmp(argument, "box") ||
        !strcasecmp(argument, "pockets") || !strcasecmp(argument, "wallet") ||
        !strcasecmp(argument, "room")) {
        char *items = r2_reality_list(argument);
        printf("%s:\n%s", argument, items ? items : "(could not read container)\n");
        free(items);
        return 1;
    }
    /* Unknown room subcommands are treated as named container lookups,
       so commands such as "room toy box" inspect custom containers. */
    char *items = r2_reality_list(argument);
    printf("%s:\n%s", argument, items ? items : "(could not read container)\n");
    free(items);
    return 1;
}

static int shell_pockets(const char *argument)
{
    if (!argument || !*argument) return shell_reality("pockets");
    if (!strcasecmp(argument, "wallet")) return shell_reality("wallet");
    if (shell_starts_with(argument, "put ")) {
        char command[8192];
        snprintf(command, sizeof(command), "move %s | pockets", argument + 4);
        return shell_reality(command);
    }
    if (shell_starts_with(argument, "wallet ")) {
        char command[8192];
        snprintf(command, sizeof(command), "move %s | wallet", argument + 7);
        return shell_reality(command);
    }
    if (shell_starts_with(argument, "take ")) {
        char *copy = strdup(argument + 5);
        if (!copy) return 1;
        char *sep = strchr(copy, '|');
        if (sep) *sep++ = '\0';
        char command[8192];
        snprintf(command, sizeof(command), "move %s | %s", reality_trim(copy),
                 sep && *reality_trim(sep) ? reality_trim(sep) : "room");
        int rc = shell_reality(command);
        free(copy);
        return rc;
    }
    printf("Usage: pockets [wallet|put <name>|wallet <name>|take <name> [| container]]\n");
    return 1;
}




static int shell_fridge(const char *argument)
{
    if (!argument || !*argument || !strcasecmp(argument, "look")) {
        char *items = r2_fridge_context();
        printf("%s\n", items ? items : "[R2 Fridge] Database unavailable.");
        free(items);
        return 1;
    }
    if (shell_starts_with(argument, "take ")) {
        const char *food = reality_trim((char *)argument + 5);
        int rc = r2_fridge_take(food);
        printf(rc == 0 ? "[R2 Fridge] Moved one %s into pockets.\n" :
                         "[R2 Fridge] Could not take that item.\n", food);
        return 1;
    }
    if (shell_starts_with(argument, "eat ")) {
        const char *food = reality_trim((char *)argument + 4);
        int rc = r2_eat_fridge_and_learn(food, -1.0);
        printf(rc == 0 ? "[R2 Fridge] Ate %s from fridge stock; hunger updated.\n" :
                         "[R2 Fridge] Could not eat that item from the fridge.\n", food);
        return 1;
    }
    if (shell_starts_with(argument, "store ")) {
        const char *food = reality_trim((char *)argument + 6);
        int rc = r2_reality_move_item(food, "fridge");
        printf(rc == 0 ? "[R2 Fridge] Stored %s from pockets/inventory into the fridge.\n" :
                         "[R2 Fridge] Could not store that tracked item.\n", food);
        return 1;
    }
    printf("Usage: fridge [look|take <food>|eat <food>|store <food>]\n");
    return 1;
}

static int shell_parse_amount(const char *text, double *value);

static int shell_give(const char *arg)
{
    if (!arg || !*arg) {
        printf("Usage: give <item> <quantity> [| description | container]\n");
        printf("       give money <quantity> creates separate physical money items.\n");
        return 1;
    }
    char *copy = strdup(arg);
    if (!copy) {
        printf("[Creator] Memory allocation failed.\n");
        return 1;
    }
    char *fields[3] = {0};
    int count = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, "|", &save);
         part && count < 3;
         part = strtok_r(NULL, "|", &save))
        fields[count++] = shell_trim(part);

    char *head = fields[0] ? fields[0] : copy;
    char *space = strrchr(head, ' ');
    if (!space || space == head || !space[1]) {
        printf("Usage: give <item> <quantity> [| description | container]\n");
        free(copy);
        return 1;
    }
    *space++ = '\0';
    char *name = shell_trim(head);
    char *amount_text = shell_trim(space);
    double amount = 0.0;
    if (!*name || !shell_parse_amount(amount_text, &amount) ||
        amount <= 0.0 || amount > 1000000.0 || floor(amount) != amount) {
        printf("[Give] Quantity must be a positive whole number (maximum 1000000).\n");
        free(copy);
        return 1;
    }
    const char *destination = count >= 3 && fields[2] && *fields[2]
        ? fields[2] : "pockets";
    const char *description = count >= 2 && fields[1] && *fields[1]
        ? fields[1] : "Created from nothing by the user through the creator give command";
    int rc = 0;
    int money = !strcasecmp(name, "money");
    if (money) {
        if (amount > 1000.0) {
            printf("[Creator] A single money gift is limited to 1000 separate objects.\n");
            free(copy);
            return 1;
        }
        /* One object and one .r2item mirror per unit; never interpret quantity
           as dollars or collapse separate money pieces into one stack. */
        for (int i = 1; i <= (int)amount; ++i) {
            char item_name[96], item_description[512];
            snprintf(item_name, sizeof(item_name), "money gift %ld %ld %d",
                     (long)time(NULL), (long)getpid(), i);
            snprintf(item_description, sizeof(item_description),
                     "Individual physical money item created by the user; denomination unspecified. Gift batch quantity=%d.",
                     (int)amount);
            if (r2_reality_add_item(item_name, item_description, destination, 1) != 0) {
                fprintf(stderr, "[Creator] Could not create %s in %s; stopping after %d of %.0f items.\n",
                        item_name, destination, i - 1, amount);
                rc = -1;
                break;
            }
        }
        if (rc == 0)
            printf("[Creator] Created %.0f separate money items in %s; no cash balance or denomination was assumed.\n",
                   amount, destination);
    } else {
        rc = r2_reality_add_item(name, description, destination, (int)amount);
        if (rc == 0)
            printf("[Creator] Created %d x %s in %s.\n", (int)amount, name, destination);
        else
            printf("[Creator] Could not create %s in %s; check location and container access.\n",
                   name, destination);
    }
    if (rc == 0 && r2_log_is_initialized()) {
        char summary[512], details[1024];
        if (money) {
            snprintf(summary, sizeof(summary), "The user created %.0f separate physical money items for R2.", amount);
            snprintf(details, sizeof(details),
                     "Creator command=give money; individual item count=%.0f; destination=%s; physical inventory objects only; no dollar value, cash balance, purchase, or denomination was assumed.",
                     amount, destination);
        } else {
            snprintf(summary, sizeof(summary), "The user created a gift for R2: %s x %.0f.", name, amount);
            snprintf(details, sizeof(details),
                     "Creator command=give; target=%s; quantity=%.0f; destination=%s; source=explicit user creator action; no purchase or price was involved.",
                     name, amount, destination);
        }
        (void)r2_log_event_with_memory(R2_LOG_WORLD, "creator_gift", summary, details, "shell.c", 1);
    }
    free(copy);
    return 1;
}

typedef struct {
    int valid;
    int running;
    char power_state[16];
    char title[256];
    char session_id[160];
} ShellGameboyStatus;

/* Read authoritative device state without showing JSON to the user. */
static int shell_gameboy_read_status(ShellGameboyStatus *out)
{
    const char *database = "/home/x/R2_Home/Devices/GameBoyAdvance/State/console_state.db";
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    int ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 1000);
    const char *sql =
        "SELECT s.power_state,s.emulator_pid,s.session_id,"
        "(SELECT e.game_title FROM console_events e "
        " WHERE e.session_id=s.session_id AND e.game_title IS NOT NULL "
        " ORDER BY e.id DESC LIMIT 1) "
        "FROM console_state s WHERE s.id=1";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *power = sqlite3_column_text(statement, 0);
        const unsigned char *session = sqlite3_column_text(statement, 2);
        const unsigned char *title = sqlite3_column_text(statement, 3);
        int has_pid = sqlite3_column_type(statement, 1) != SQLITE_NULL &&
                      sqlite3_column_int(statement, 1) > 1;
        if (power)
            snprintf(out->power_state, sizeof(out->power_state), "%s", (const char *)power);
        if (session)
            snprintf(out->session_id, sizeof(out->session_id), "%s", (const char *)session);
        if (title)
            snprintf(out->title, sizeof(out->title), "%s", (const char *)title);
        out->running = power && strcmp((const char *)power, "on") == 0 && has_pid;
        out->valid = out->power_state[0] != '\0';
        ok = out->valid;
    }
    if (statement) sqlite3_finalize(statement);
    sqlite3_close(db);
    return ok;
}

static void shell_gameboy_log_session_start(const ShellGameboyStatus *state)
{
    char key[256], name[512], details[1200], summary[1200];
    if (!state || !state->running || !state->session_id[0]) return;
    snprintf(key, sizeof(key), "gameboy:%s", state->session_id);
    snprintf(name, sizeof(name), "Playing %s on Game Boy Advance",
             state->title[0] ? state->title : "a game");
    snprintf(details, sizeof(details),
             "Real-life activity: R2 started a game session. session_id=%s; "
             "the game's virtual events remain separate from physical Reality.",
             state->session_id);
    if (r2_log_activity_start(key, name, details) < 0) {
        snprintf(summary, sizeof(summary), "R2 began playing %s on the Game Boy Advance.",
                 state->title[0] ? state->title : "a game");
        (void)r2_log_event(R2_LOG_MEDIA, "game_session_started", summary,
                           details, "GameBoyAdvance.py");
    }
}

static void shell_gameboy_log_session_end(const ShellGameboyStatus *state)
{
    char key[256], details[1200], summary[1200];
    const char *title;
    if (!state || !state->running || !state->session_id[0]) return;
    title = state->title[0] ? state->title : "a game";
    snprintf(key, sizeof(key), "gameboy:%s", state->session_id);
    snprintf(details, sizeof(details),
             "Real-life activity ended because the console session stopped. "
             "session_id=%s; in-game events are not physical-life events.",
             state->session_id);
    if (r2_log_activity_end(key, "Game Boy Advance emulator stopped",
                            "console_powered_off", details) < 0) {
        snprintf(summary, sizeof(summary), "R2 stopped playing %s on the Game Boy Advance.", title);
        (void)r2_log_event(R2_LOG_MEDIA, "game_session_ended", summary,
                           details, "GameBoyAdvance.py");
    }
}

static int shell_gameboy(const char *arg)
{
    const char *device = "/home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance";
    const char *sub = (arg && *arg) ? arg : "power on";
    char *copy = strdup(sub);
    if (!copy) {
        printf("[Game Boy Advance] Could not allocate command arguments.\n");
        return 1;
    }

    /* execv avoids interpreting user-supplied subcommands through a shell. */
    char *argv[10] = {0};
    int argc = 0;
    char *save = NULL;
    char *part = strtok_r(copy, " \t", &save);
    while (part) {
        if (argc >= 8) {
            printf("[Game Boy Advance] Too many command arguments.\n");
            free(copy);
            return 1;
        }
        argv[++argc] = part;
        part = strtok_r(NULL, " \t", &save);
    }
    if (argc == 0) {
        argv[0] = (char *)device;
        argv[1] = "power";
        argv[2] = "on";
        argc = 2;
    } else {
        argv[0] = (char *)device;
    }
    argv[argc + 1] = NULL;

    ShellGameboyStatus before = {0}, after = {0};
    int have_before = shell_gameboy_read_status(&before);

    pid_t child = fork();
    if (child < 0) {
        perror("[Game Boy Advance] fork");
        free(copy);
        return 1;
    }
    if (child == 0) {
        execv(device, argv);
        perror("[Game Boy Advance] execv");
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("[Game Boy Advance] waitpid");
        free(copy);
        return 1;
    }
    int command_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!command_ok) {
        printf("[Game Boy Advance] Command failed. Check installation, inserted ROM, and mGBA path.\n");
    } else if (have_before && shell_gameboy_read_status(&after)) {
        /* Compare the persistent session IDs, not just the command name. This
           also closes Life Log activities when a later status/verify command
           discovers that mGBA exited unexpectedly. */
        if (before.session_id[0] &&
            (!after.session_id[0] || strcmp(before.session_id, after.session_id) != 0))
            shell_gameboy_log_session_end(&before);
        if (after.session_id[0] &&
            (!before.session_id[0] || strcmp(before.session_id, after.session_id) != 0))
            shell_gameboy_log_session_start(&after);
    }
    free(copy);
    return 1;
}

static int shell_parse_amount(const char *text, double *value)
{
    if (!text || !value) return 0;
    errno = 0;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(parsed)) return 0;
    while (end && *end && isspace((unsigned char)*end)) ++end;
    if (end && *end) return 0;
    *value = parsed;
    return 1;
}

static int shell_money(const char *arg)
{
    if(!arg||!*arg||!strcasecmp(arg,"status")){char *s=r2_reality_money_context();printf("%s\n",s?s:"[R2 Money] Unavailable.");free(s);return 1;}
    if(shell_starts_with(arg,"receive ")){double a=0.0;int rc=shell_parse_amount(arg+8,&a)?r2_reality_money_receive(a):-1;printf(rc==0?"Recorded $%.2f received as cash.\n":"Could not record money received; enter a valid positive amount.\n",a);return 1;}
    if(shell_starts_with(arg,"deposit ")){double a=0.0;int rc=shell_parse_amount(arg+8,&a)?r2_reality_money_deposit(a):-1;printf(rc==0?"Deposited $%.2f into piggybank.\n":"Deposit failed; enter a valid amount and check cash.\n",a);return 1;}
    if(shell_starts_with(arg,"withdraw ")){double a=0.0;int rc=shell_parse_amount(arg+9,&a)?r2_reality_money_withdraw(a):-1;printf(rc==0?"Withdrew $%.2f into cash.\n":"Withdrawal failed; enter a valid amount and check bank balance.\n",a);return 1;}
    if(shell_starts_with(arg,"buy ")){char *copy=strdup(arg+4);if(!copy)return 1;char *p[4]={0};int n=0;char *save=NULL;
        for(char *t=strtok_r(copy,"|",&save);t&&n<4;t=strtok_r(NULL,"|",&save))p[n++]=shell_trim(t);
        if(n<2||!*p[0]||!*p[1])printf("Usage: money buy <item> | <price> | <description> | <container>\n");
        else{double price=0.0;int parsed=shell_parse_amount(p[1],&price);int rc=parsed?r2_reality_buy_item(p[0],n>=3&&*p[2]?p[2]:"Purchased item",price,n>=4&&*p[3]?p[3]:"pockets"):-1;printf(rc==0?"Purchased %s for $%.2f.\n":"Purchase failed; enter a valid price and check funds/destination.\n",p[0],price);}free(copy);return 1;}
    printf("Usage: money [status|deposit <amount>|withdraw <amount>|buy <item> | <price> | <description> | <container>]\n");return 1;
}

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


static int shell_needs(const char *command)
{
    if (shell_starts_with(command, "eat ")) {
        char *copy = strdup(command + 4);
        if (!copy) return 1;
        char *sep = strchr(copy, '|');
        char *food = copy;
        double fullness = -1.0;
        if (sep) {
            *sep = '\0';
            fullness = atof(reality_trim(sep + 1));
        }
        food = reality_trim(food);
        if (!*food) {
            printf("Usage: eat <food> [| <fullness 0-100>]\n");
        } else {
            int rc = r2_eat_and_learn(food, fullness);
            printf(rc == 0 ? "[R2 Reality] Food and hunger state updated.\n" :
                             "[R2 Reality] Eating update failed; check room/food_metrics.xml or supply fullness points.\n");
        }
        free(copy);
        return 1;
    }
    if (shell_starts_with(command, "sleep ")) {
        double hours = atof(command + 6);
        int rc = r2_sleep_and_dream(hours);
        printf(rc == 0 ? "[R2 Reality] Sleep transition persisted; dream simulation was attempted.\n" :
                         "[R2 Reality] Sleep update failed; use a duration from 0 to 48 hours.\n");
        return 1;
    }
    if (shell_starts_with(command, "dream ")) {
        int rc = r2_reality_record_dream(command + 6);
        printf(rc == 0 ? "[R2 Reality] Dream report recorded.\n" :
                         "[R2 Reality] Dream report could not be recorded.\n");
        return 1;
    }
    return 0;
}

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
       PREFERENCE / ADDICTION HISTORY
       -------------------------------------------------------- */
    if (!strcasecmp(command, "addictions") || !strcasecmp(command, "addiction")) {
        shell_addictions();
        return 1;
    }

    /* --------------------------------------------------------
       DIARY
       -------------------------------------------------------- */

    if (!strcasecmp(command, "diary")) {
        shell_diary();
        return 1;
    }



    if (shell_starts_with(command, "eat ") ||
        shell_starts_with(command, "sleep ") ||
        shell_starts_with(command, "dream ")) {
        shell_needs(command);
        return 1;
    }

    /* --------------------------------------------------------
       CREATOR GIFTS AND VIRTUAL GAME BOY
       -------------------------------------------------------- */

    if (!strcasecmp(command, "give") || shell_starts_with(command, "give ")) {
        shell_give(shell_starts_with(command, "give ") ? shell_trim(command + 5) : NULL);
        return 1;
    }
    if (!strcasecmp(command, "tv") || shell_starts_with(command, "tv ")) {
        shell_tv(shell_starts_with(command, "tv ") ? shell_trim(command + 3) : NULL);
        return 1;
    }
    if (!strcasecmp(command, "gameboy") || shell_starts_with(command, "gameboy ")) {
        shell_gameboy(shell_starts_with(command, "gameboy ") ? shell_trim(command + 8) : NULL);
        return 1;
    }

    /* --------------------------------------------------------
       PERSISTENT REALITY / WORLD
       -------------------------------------------------------- */
    if (!strcasecmp(command, "room") || shell_starts_with(command, "room ")) {
        shell_reality(shell_starts_with(command, "room ") ? shell_trim(command + 5) : NULL);
        return 1;
    }
    if (!strcasecmp(command, "pockets") || shell_starts_with(command, "pockets ")) {
        shell_pockets(shell_starts_with(command, "pockets ") ? shell_trim(command + 8) : NULL);
        return 1;
    }
    if (!strcasecmp(command, "fridge") || shell_starts_with(command, "fridge ")) {
        shell_fridge(shell_starts_with(command, "fridge ") ? shell_trim(command + 7) : NULL);
        return 1;
    }
    if(!strcasecmp(command,"money")||shell_starts_with(command,"money ")){shell_money(shell_starts_with(command,"money ")?shell_trim(command+6):NULL);return 1;}
    if (!strcasecmp(command, "alternate")) { shell_alternate(NULL); return 1; }
    if (shell_starts_with(command, "alternate ")) { shell_alternate(shell_trim(command + 10)); return 1; }
    if (!strcasecmp(command, "world status")) {
        shell_reality("status");
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

    if (tv_control_start() != 0)
        fprintf(stderr, "[TV] GUI control socket unavailable; shell TV commands remain available.\n");

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
    if (watch_running) {
        r2_watch_stop();
        watch_running = 0;
    }

    shell_running = 0;
    shell_shutdown = 1;
}
