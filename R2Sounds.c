#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "R2Sounds.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
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
                /* Common aliases allow the caller's state to match a named
                   variation without making arbitrary substring matches. */
                if ((!strcmp(requested, "curious") && token_in(tokens, count, "curiosity")) ||
                    (!strcmp(requested, "happy") && (token_in(tokens, count, "joy") || token_in(tokens, count, "cheerful"))) ||
                    (!strcmp(requested, "confused") && token_in(tokens, count, "puzzled")) ||
                    (!strcmp(requested, "sleepy") && (token_in(tokens, count, "tired") || token_in(tokens, count, "drowsy"))) ||
                    (!strcmp(requested, "thinking") && (token_in(tokens, count, "pondering") || token_in(tokens, count, "processing"))) ||
                    (!strcmp(requested, "greeting") && (token_in(tokens, count, "hello") || token_in(tokens, count, "welcome"))))
                    return 90;
            }
            n = 0;
        }
        if (!*p) break;
    }
    return 0;
}

static int token_in(char tokens[][R2_SOUND_TOKEN_MAX], int count,
                    const char *wanted)
{
    for (int i = 0; i < count; ++i)
        if (!strcmp(tokens[i], wanted)) return 1;
    return 0;
}

static int filename_score(const char *name, const char *kind, const char *state)
{
    char tokens[64][R2_SOUND_TOKEN_MAX];
    int count = tokenize_filename(name, tokens, 64);
    int has_kind = 0, generic = 0;
    for (int i = 0; i < count; ++i) {
        if (token_is_kind(tokens[i], kind)) has_kind = 1;
        if (!strcmp(tokens[i], "default") || !strcmp(tokens[i], "generic") ||
            !strcmp(tokens[i], "normal") || !strcmp(tokens[i], "idle") ||
            !strcmp(tokens[i], "neutral"))
            generic = 1;
    }
    if (!has_kind) return -1;
    int exact_state = state_score(tokens, count, state);
    if (exact_state) return 1000 + exact_state;
    if (generic) return 200;
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

static void player_child(const char *path)
{
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd >= 0) {
        (void)dup2(nullfd, STDOUT_FILENO);
        (void)dup2(nullfd, STDERR_FILENO);
        if (nullfd > STDERR_FILENO) close(nullfd);
    }
    execlp("mpg123", "mpg123", "-q", path, (char *)NULL);
    execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
           path, (char *)NULL);
    execlp("mpv", "mpv", "--no-video", "--really-quiet", path, (char *)NULL);
    _exit(127);
}

int r2_sounds_play(const char *kind, const char *state)
{
    const char *dir = getenv("R2_FX_DIR");
    if (!dir || !*dir) dir = R2_SOUNDS_DEFAULT_FX_DIR;
    char path[R2_SOUND_PATH_MAX];
    if (r2_sounds_select_file(dir, kind, state, path, sizeof(path)) != 0)
        return -1;

    pthread_mutex_lock(&sound_lock);
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
        pid_t result = waitpid(sound_child, &status, WNOHANG);
        if (result == 0) {
            pthread_mutex_unlock(&sound_lock);
            return 1; /* Don't overlap an effect already playing. */
        }
        sound_child = -1;
    }
    pid_t child = fork();
    if (child < 0) {
        pthread_mutex_unlock(&sound_lock);
        return -1;
    }
    if (child == 0) player_child(path);
    sound_child = child;
    last_sound_at = now;
    have_last_sound_at = 1;
    pthread_mutex_unlock(&sound_lock);
    return 0;
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

char *r2_sounds_process_reply(const char *reply)
{
    if (!reply) return NULL;
    size_t len = strlen(reply);
    char *clean = malloc(len + 1);
    if (!clean) return NULL;
    size_t src = 0, dst = 0;
    int played_or_attempted = 0;
    while (src < len) {
        const char *marker = strstr(reply + src, "[R2_SOUND:");
        if (!marker) {
            memcpy(clean + dst, reply + src, len - src);
            dst += len - src;
            break;
        }
        size_t prefix_len = (size_t)(marker - (reply + src));
        memcpy(clean + dst, reply + src, prefix_len);
        dst += prefix_len;
        const char *end = strchr(marker, ']');
        if (!end || (size_t)(end - marker) >= R2_SOUND_MARKER_MAX) {
            /* Malformed marker: remove only the control prefix, not the prose. */
            src = (size_t)(marker - reply) + strlen("[R2_SOUND:");
            continue;
        }
        char inside[R2_SOUND_MARKER_MAX];
        size_t inside_len = (size_t)(end - (marker + strlen("[R2_SOUND:")));
        memcpy(inside, marker + strlen("[R2_SOUND:"), inside_len);
        inside[inside_len] = '\0';
        char *sep = strchr(inside, ':');
        if (sep) {
            *sep++ = '\0';
            if (!played_or_attempted && valid_marker_value(inside, 16) &&
                valid_marker_value(sep, 48) &&
                (!strcasecmp(inside, "beep") || !strcasecmp(inside, "whistle"))) {
                (void)r2_sounds_play(inside, sep);
                played_or_attempted = 1;
            }
        }
        src = (size_t)(end - reply) + 1;
    }
    clean[dst] = '\0';
    return clean;
}
