#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "R2Sounds.h"
#include "Log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define R2_SOUND_PATH_MAX 4096
#define R2_SOUND_TOKEN_MAX 64
#define R2_SOUND_MARKER_MAX 160
#define R2_SOUND_MIN_INTERVAL_MS 900

static pthread_mutex_t sound_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t sound_child = -1;
static struct timespec last_sound_at;
static int have_last_sound_at;
static int sound_shutting_down;

static int is_mp3_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 4 && name[n - 4] == '.' &&
           tolower((unsigned char)name[n - 3]) == 'm' &&
           tolower((unsigned char)name[n - 2]) == 'p' &&
           tolower((unsigned char)name[n - 1]) == '3';
}

static int token_is_state(const char *token)
{
    static const char *const states[] = {
        "curious", "curiosity", "happy", "happiness", "joy", "cheerful",
        "pleased", "confused", "confusion", "uncertain", "puzzled",
        "alert", "attention", "warning", "alarm", "sad", "sadness",
        "disappointed", "sleepy", "tired", "exhausted", "drowsy",
        "hungry", "hunger", "excited", "excitement", "enthusiastic",
        "thinking", "think", "processing", "pondering", "greeting",
        "greet", "hello", "welcome", "acknowledge", "acknowledgement",
        "confirm", "affirmative", "yes", "neutral", "default", "idle",
        "normal", "generic", "calm", "playful", "worried", "surprised",
        "content", "frustrated", "sleep", "resting"
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
        if (!strcmp(token, states[i])) return 1;
    return 0;
}

static int token_is_generic_state(const char *token)
{
    return token && (!strcmp(token, "default") || !strcmp(token, "generic") ||
                     !strcmp(token, "normal") || !strcmp(token, "idle") ||
                     !strcmp(token, "neutral"));
}

static int token_is_kind(const char *token, const char *kind)
{
    if (!strcmp(kind, "beep"))
        return !strcmp(token, "beep") || !strcmp(token, "beeps") ||
               !strcmp(token, "boop") || !strcmp(token, "boops") ||
               !strcmp(token, "bleep") || !strcmp(token, "bleeps");
    if (!strcmp(kind, "whistle"))
        return !strcmp(token, "whistle") || !strcmp(token, "whistles") ||
               !strcmp(token, "whistling");
    return 0;
}

static int tokenize_filename(const char *name, char tokens[][R2_SOUND_TOKEN_MAX],
                             size_t max_tokens)
{
    size_t count = 0, length = 0;
    char token[R2_SOUND_TOKEN_MAX];
    if (!name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; ; ++p) {
        int is_word = *p && isalnum(*p);
        if (is_word) {
            if (length + 1 < sizeof(token))
                token[length++] = (char)tolower(*p);
            continue;
        }
        if (length) {
            token[length] = '\0';
            if (count < max_tokens) {
                snprintf(tokens[count], R2_SOUND_TOKEN_MAX, "%s", token);
                ++count;
            }
            length = 0;
        }
        if (!*p) break;
    }
    return (int)count;
}

static int state_alias_match(const char *requested,
                            char tokens[][R2_SOUND_TOKEN_MAX], int count);

static int state_score(char tokens[][R2_SOUND_TOKEN_MAX], int count,
                       const char *state)
{
    char requested[R2_SOUND_TOKEN_MAX];
    size_t n = 0;
    if (!state || !*state) return 0;
    for (const unsigned char *p = (const unsigned char *)state; ; ++p) {
        if (*p && isalnum(*p)) {
            if (n + 1 < sizeof(requested))
                requested[n++] = (char)tolower(*p);
            continue;
        }
        if (n) {
            requested[n] = '\0';
            if (token_is_state(requested)) {
                for (int i = 0; i < count; ++i)
                    if (!strcmp(tokens[i], requested)) return 100;
                /* Match documented synonym families symmetrically. This keeps
                   filename selection aligned with the read-only asset audit. */
                if (state_alias_match(requested, tokens, count)) return 90;
            }
            n = 0;
        }
        if (!*p) break;
    }
    return 0;
}

static int token_in_group(const char *token, const char *const *group)
{
    for (size_t i = 0; group[i]; ++i)
        if (!strcmp(token, group[i])) return 1;
    return 0;
}

static int state_alias_match(const char *requested,
                            char tokens[][R2_SOUND_TOKEN_MAX], int count)
{
    static const char *const curious[] = {"curious", "curiosity", NULL};
    static const char *const happy[] = {"happy", "happiness", "joy", "cheerful", "pleased", NULL};
    static const char *const confused[] = {"confused", "confusion", "uncertain", "puzzled", NULL};
    static const char *const alert[] = {"alert", "attention", "warning", "alarm", NULL};
    static const char *const sad[] = {"sad", "sadness", "disappointed", NULL};
    static const char *const sleepy[] = {"sleepy", "tired", "exhausted", "drowsy", NULL};
    static const char *const hungry[] = {"hungry", "hunger", NULL};
    static const char *const excited[] = {"excited", "excitement", "enthusiastic", NULL};
    static const char *const thinking[] = {"thinking", "think", "processing", "pondering", NULL};
    static const char *const greeting[] = {"greeting", "greet", "hello", "welcome", "acknowledge", "acknowledgement", NULL};
    static const char *const confirm[] = {"confirm", "affirmative", "yes", NULL};
    static const char *const neutral[] = {"neutral", "default", "idle", "normal", "generic", NULL};
    static const char *const resting[] = {"sleep", "resting", NULL};
    static const char *const *const groups[] = {
        curious, happy, confused, alert, sad, sleepy, hungry, excited,
        thinking, greeting, confirm, neutral, resting, NULL
    };
    for (size_t g = 0; groups[g]; ++g) {
        if (!token_in_group(requested, groups[g])) continue;
        for (int i = 0; i < count; ++i)
            if (token_in_group(tokens[i], groups[g])) return 1;
    }
    return 0;
}

static int filename_score(const char *name, const char *kind, const char *state)
{
    char tokens[64][R2_SOUND_TOKEN_MAX];
    int count = tokenize_filename(name, tokens, 64);
    int has_kind = 0, generic = 0, has_other_state = 0;
    for (int i = 0; i < count; ++i) {
        if (token_is_kind(tokens[i], kind)) has_kind = 1;
        if (token_is_generic_state(tokens[i]))
            generic = 1;
        else if (token_is_state(tokens[i]))
            has_other_state = 1;
    }
    if (!has_kind) return -1;
    int exact_state = state_score(tokens, count, state);
    if (exact_state) return 1000 + exact_state;
    /* A generic suffix cannot override a conflicting explicit state label. */
    if (has_other_state) return -1;
    if (generic) return 200;
    /* Only an untagged kind-specific file can serve as the last resort. */
    return 100;
}

int r2_sounds_select_file(const char *fx_dir, const char *kind,
                          const char *state, char *out, size_t out_cap)
{
    if (!out || !out_cap) return -1;
    out[0] = '\0';
    if (!fx_dir || !*fx_dir || !kind) return -1;
    char normalized_kind[16];
    if (!strcasecmp(kind, "beep") || !strcasecmp(kind, "boop") ||
        !strcasecmp(kind, "bleep"))
        snprintf(normalized_kind, sizeof(normalized_kind), "beep");
    else if (!strcasecmp(kind, "whistle"))
        snprintf(normalized_kind, sizeof(normalized_kind), "whistle");
    else return -1;

    DIR *dir = opendir(fx_dir);
    if (!dir) return -1;
    struct dirent *entry;
    char best_name[NAME_MAX + 1] = {0};
    int best_score = -1;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_mp3_name(entry->d_name)) continue;
        char candidate[R2_SOUND_PATH_MAX];
        int candidate_len = snprintf(candidate, sizeof(candidate), "%s/%s",
                                     fx_dir, entry->d_name);
        struct stat file_info;
        if (candidate_len < 0 || (size_t)candidate_len >= sizeof(candidate) ||
            stat(candidate, &file_info) != 0 || !S_ISREG(file_info.st_mode))
            continue;
        int score = filename_score(entry->d_name, normalized_kind, state);
        if (score < 0) continue;
        if (score > best_score ||
            (score == best_score && (!*best_name || strcmp(entry->d_name, best_name) < 0))) {
            best_score = score;
            snprintf(best_name, sizeof(best_name), "%s", entry->d_name);
        }
    }
    closedir(dir);
    if (!*best_name) return -1;
    int written = snprintf(out, out_cap, "%s/%s", fx_dir, best_name);
    if (written < 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
    return (long)(b->tv_sec - a->tv_sec) * 1000L +
           (long)(b->tv_nsec - a->tv_nsec) / 1000000L;
}

extern char **environ;

/* posix_spawn avoids running non-async-signal-safe libc code after fork in
   R2's multithreaded process. Try the local decoders without a shell. */
static pid_t spawn_player(const char *path)
{
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }

    pid_t child = -1;
    char *mpg123_argv[] = {"mpg123", "-q", (char *)path, NULL};
    int rc = posix_spawnp(&child, "mpg123", &actions, NULL, mpg123_argv, environ);
    if (rc == 0) {
        posix_spawn_file_actions_destroy(&actions);
        return child;
    }

    char *ffplay_argv[] = {"ffplay", "-nodisp", "-autoexit", "-loglevel",
                           "quiet", (char *)path, NULL};
    rc = posix_spawnp(&child, "ffplay", &actions, NULL, ffplay_argv, environ);
    if (rc == 0) {
        posix_spawn_file_actions_destroy(&actions);
        return child;
    }

    char *mpv_argv[] = {"mpv", "--no-video", "--really-quiet", (char *)path, NULL};
    rc = posix_spawnp(&child, "mpv", &actions, NULL, mpv_argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 ? child : -1;
}

void r2_sounds_init(void)
{
    pthread_mutex_lock(&sound_lock);
    /* A previous orderly shutdown reaps and clears the child. Do not reset
       shutdown protection over a live player if initialization is miscalled. */
    if (sound_child <= 0) {
        sound_shutting_down = 0;
        have_last_sound_at = 0;
        memset(&last_sound_at, 0, sizeof(last_sound_at));
    }
    pthread_mutex_unlock(&sound_lock);
}

int r2_sounds_play(const char *kind, const char *state)
{
    const char *dir = getenv("R2_FX_DIR");
    if (!dir || !*dir) dir = R2_SOUNDS_DEFAULT_FX_DIR;
    char path[R2_SOUND_PATH_MAX];
    if (r2_sounds_select_file(dir, kind, state, path, sizeof(path)) != 0)
        return -1;

    pthread_mutex_lock(&sound_lock);
    if (sound_shutting_down) {
        pthread_mutex_unlock(&sound_lock);
        return -1;
    }
    if (getenv("R2_SOUNDS_DISABLE_PLAYBACK") &&
        !strcmp(getenv("R2_SOUNDS_DISABLE_PLAYBACK"), "1")) {
        pthread_mutex_unlock(&sound_lock);
        return 0;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        pthread_mutex_unlock(&sound_lock);
        return -1;
    }
    if (have_last_sound_at && elapsed_ms(&last_sound_at, &now) < R2_SOUND_MIN_INTERVAL_MS) {
        pthread_mutex_unlock(&sound_lock);
        return 1;
    }
    if (sound_child > 0) {
        int status = 0;
        pid_t result;
        do {
            result = waitpid(sound_child, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            pthread_mutex_unlock(&sound_lock);
            return 1; /* Don't overlap an effect already playing. */
        }
        sound_child = -1;
    }
    pid_t child = spawn_player(path);
    if (child < 0) {
        pthread_mutex_unlock(&sound_lock);
        return -1;
    }
    sound_child = child;
    last_sound_at = now;
    have_last_sound_at = 1;
    pthread_mutex_unlock(&sound_lock);

    /* Keep sound output in the existing factual Life Log. This records that
       playback was scheduled, not that hardware or a decoder confirmed sound. */
    if (r2_log_is_initialized()) {
        const char *filename = strrchr(path, '/');
        filename = filename ? filename + 1 : path;
        char details[512];
        snprintf(details, sizeof(details), "kind=%.16s; state=%.64s; file=%.200s",
                 kind, (state && *state) ? state : "unspecified", filename);
        (void)r2_log_event(R2_LOG_SENSORY, "sound_effect_scheduled",
                           "R2 scheduled a named local droid sound effect.",
                           details, "R2Sounds");
    }
    return 0;
}

void r2_sounds_shutdown(void)
{
    pthread_mutex_lock(&sound_lock);
    sound_shutting_down = 1;
    pid_t child = sound_child;
    sound_child = -1;
    have_last_sound_at = 0;
    pthread_mutex_unlock(&sound_lock);

    if (child <= 0) return;
    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result != 0) return;

    (void)kill(child, SIGTERM);
    /* Never let a broken decoder hang the robot's entire shutdown path. */
    for (int attempt = 0; attempt < 20; ++attempt) {
        struct timespec delay = {0, 25000000L};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        do {
            result = waitpid(child, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result != 0) return;
    }

    (void)kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

static int valid_marker_value(const char *value, size_t max_len)
{
    size_t n = value ? strlen(value) : 0;
    if (!n || n >= max_len) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)value[i]) &&
            value[i] != '_' && value[i] != '-') return 0;
    return 1;
}

/* Unknown model-invented labels must not trigger even a generic sound. */
static int valid_marker_state(const char *value)
{
    char normalized[R2_SOUND_MARKER_MAX];
    size_t n = value ? strlen(value) : 0;
    if (!n || n >= sizeof(normalized)) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == '_' || ch == '-') normalized[i] = ' ';
        else if (isalnum(ch)) normalized[i] = (char)tolower(ch);
        else return 0;
    }
    normalized[n] = '\0';
    char tokens[2][R2_SOUND_TOKEN_MAX];
    int count = tokenize_filename(normalized, tokens, 2);
    return count == 1 && token_is_state(tokens[0]);
}

static int sound_fence_line(const char *line, size_t length,
                            char expected_char, size_t expected_length,
                            int closing, char *found_char, size_t *found_length)
{
    size_t pos = 0;
    while (pos < length && line[pos] == ' ' && pos < 4)
        ++pos;
    if (pos > 3 || pos >= length ||
        (line[pos] != '`' && line[pos] != '~'))
        return 0;

    char ch = line[pos];
    size_t run = 0;
    while (pos + run < length && line[pos + run] == ch)
        ++run;
    if (run < 3)
        return 0;

    if (closing) {
        if (ch != expected_char || run < expected_length)
            return 0;
        pos += run;
        while (pos < length && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
        if (pos != length)
            return 0;
    } else if (ch == '`') {
        /* Markdown forbids backticks in a backtick-fence info string. */
        for (size_t i = pos + run; i < length; ++i)
            if (line[i] == '`')
                return 0;
    }

    if (found_char) *found_char = ch;
    if (found_length) *found_length = run;
    return 1;
}

static char *process_reply_markers(const char *reply, int play_sound)
{
    if (!reply) return NULL;
    size_t len = strlen(reply);
    char *clean = malloc(len + 1);
    if (!clean) return NULL;

    size_t src = 0, dst = 0;
    size_t inline_ticks = 0;
    int in_fence = 0;
    char fence_char = '\0';
    size_t fence_length = 0;
    int marker_selected = 0;
    char selected_kind[16] = {0};
    char selected_state[R2_SOUND_MARKER_MAX] = {0};
    static const char marker_text[] = "[R2_SOUND:";
    const size_t marker_length = sizeof(marker_text) - 1;

    while (src < len) {
        size_t line_end = src;
        while (line_end < len && reply[line_end] != '\n')
            ++line_end;
        size_t line_length = line_end - src;
        char found_fence_char = '\0';
        size_t found_fence_length = 0;

        if (in_fence) {
            memcpy(clean + dst, reply + src, line_length);
            dst += line_length;
            if (sound_fence_line(reply + src, line_length, fence_char,
                                 fence_length, 1, NULL, NULL))
                in_fence = 0;
        } else if (inline_ticks == 0 &&
                   sound_fence_line(reply + src, line_length, '\0', 0, 0,
                                    &found_fence_char, &found_fence_length)) {
            /* Fenced examples are literal text, never hidden actions. */
            memcpy(clean + dst, reply + src, line_length);
            dst += line_length;
            in_fence = 1;
            fence_char = found_fence_char;
            fence_length = found_fence_length;
        } else {
            size_t pos = src;
            while (pos < line_end) {
                if (reply[pos] == '`') {
                    size_t run = 1;
                    while (pos + run < line_end && reply[pos + run] == '`')
                        ++run;
                    memcpy(clean + dst, reply + pos, run);
                    dst += run;
                    if (inline_ticks == 0)
                        inline_ticks = run;
                    else if (inline_ticks == run)
                        inline_ticks = 0;
                    pos += run;
                    continue;
                }

                if (inline_ticks == 0 && line_end - pos >= marker_length &&
                    !strncasecmp(reply + pos, marker_text, marker_length)) {
                    const char *end = memchr(reply + pos, ']', line_end - pos);
                    if (!end) {
                        /* A malformed marker is control syntax, not prose. */
                        pos = line_end;
                        break;
                    }
                    size_t marker_span = (size_t)(end - (reply + pos));
                    if (marker_span >= R2_SOUND_MARKER_MAX) {
                        pos = (size_t)(end - reply) + 1;
                        continue;
                    }

                    char inside[R2_SOUND_MARKER_MAX];
                    size_t inside_length =
                        (size_t)(end - (reply + pos + marker_length));
                    memcpy(inside, reply + pos + marker_length, inside_length);
                    inside[inside_length] = '\0';
                    char *separator = strchr(inside, ':');
                    if (separator) {
                        *separator++ = '\0';
                        if (!marker_selected &&
                            valid_marker_value(inside, 16) &&
                            valid_marker_state(separator) &&
                            (!strcasecmp(inside, "beep") ||
                             !strcasecmp(inside, "whistle"))) {
                            if (!strcasecmp(inside, "beep"))
                                memcpy(selected_kind, "beep", sizeof("beep"));
                            else
                                memcpy(selected_kind, "whistle", sizeof("whistle"));
                            snprintf(selected_state, sizeof(selected_state), "%s",
                                     separator);
                            marker_selected = 1;
                        }
                    }
                    pos = (size_t)(end - reply) + 1;
                    continue;
                }

                clean[dst++] = reply[pos++];
            }
        }

        if (line_end < len)
            clean[dst++] = reply[line_end];
        src = line_end < len ? line_end + 1 : len;
    }
    clean[dst] = '\0';

    int has_user_text = 0;
    for (size_t i = 0; i < dst; ++i) {
        if (!isspace((unsigned char)clean[i])) {
            has_user_text = 1;
            break;
        }
    }
    if (!has_user_text) {
        free(clean);
        return NULL;
    }
    /* Delay playback until we know there is an actual user-facing reply. */
    if (play_sound && marker_selected)
        (void)r2_sounds_play(selected_kind, selected_state);
    return clean;
}

char *r2_sounds_strip_markers(const char *reply)
{
    return process_reply_markers(reply, 0);
}

char *r2_sounds_process_reply(const char *reply)
{
    return process_reply_markers(reply, 1);
}
