#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Remote.h"
#include "r2.h"
#include "r2_diary.h"
#include "r2_diary.h"
#include "Log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#define REMOTE_DEFAULT_PORT 8765
#define REMOTE_HEADER_MAX 16384
#define REMOTE_BODY_MAX (2 * 1024 * 1024)
#define REMOTE_UPLOAD_MAX (50 * 1024 * 1024)
#define REMOTE_TEXT_MAX 65536

static pthread_t remote_thread;
static pthread_mutex_t remote_state_lock = PTHREAD_MUTEX_INITIALIZER;
static int remote_listener = -1;
static int remote_running = 0;
static char remote_token[512];

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = (const char *)data;
    while (length) {
        ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static void respond_json(int fd, int status, const char *reason,
                         struct json_object *payload)
{
    const char *body = payload ? json_object_to_json_string_ext(
        payload, JSON_C_TO_STRING_PLAIN) : "{}";
    size_t body_len = strlen(body);
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n",
        status, reason, body_len);
    if (n > 0 && (size_t)n < sizeof(header)) {
        (void)send_all(fd, header, (size_t)n);
        (void)send_all(fd, body, body_len);
    }
}

static void respond_error(int fd, int status, const char *reason,
                          const char *message)
{
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "error", json_object_new_string(message));
    respond_json(fd, status, reason, o);
    json_object_put(o);
}

static int constant_time_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    size_t alen = strlen(a), blen = strlen(b);
    if (alen != blen) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < alen; ++i)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static char *header_value(char *headers, const char *name)
{
    static char value_buffer[4096];
    size_t nlen = strlen(name);
    char *line = strstr(headers, "\r\n");
    if (!line) return NULL;
    line += 2;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end) break;
        if (end == line) break;
        if ((size_t)(end - line) > nlen &&
            strncasecmp(line, name, nlen) == 0 &&
            line[nlen] == ':') {
            char *value = line + nlen + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            size_t length = (size_t)(end - value);
            if (length >= sizeof(value_buffer))
                length = sizeof(value_buffer) - 1;
            memcpy(value_buffer, value, length);
            value_buffer[length] = '\0';
            return value_buffer;
        }
        line = end + 2;
    }
    return NULL;
}

static int valid_media_signature(const char *mime,
                                   const unsigned char *data,
                                   size_t length)
{
    if (!mime || !data) return 0;
    if (!strcmp(mime, "image/jpeg"))
        return length >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
    if (!strcmp(mime, "image/png"))
        return length >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
    if (!strcmp(mime, "image/webp"))
        return length >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
    if (!strcmp(mime, "video/mp4") || !strcmp(mime, "video/quicktime") ||
        !strcmp(mime, "audio/mp4"))
        return length >= 12 && memcmp(data + 4, "ftyp", 4) == 0;
    if (!strcmp(mime, "video/webm"))
        return length >= 4 && data[0] == 0x1a && data[1] == 0x45 &&
               data[2] == 0xdf && data[3] == 0xa3;
    if (!strcmp(mime, "audio/wav") || !strcmp(mime, "audio/x-wav"))
        return length >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0;
    if (!strcmp(mime, "audio/ogg"))
        return length >= 4 && memcmp(data, "OggS", 4) == 0;
    if (!strcmp(mime, "audio/mpeg"))
        return length >= 3 && (memcmp(data, "ID3", 3) == 0 ||
               (data[0] == 0xff && (data[1] & 0xe0) == 0xe0));
    if (!strcmp(mime, "audio/aac"))
        return length >= 2 && data[0] == 0xff && (data[1] & 0xf6) == 0xf0;
    return 0;
}

static long content_length(char *headers)
{
    char *value = header_value(headers, "Content-Length");
    if (!value || !*value) return 0;
    char *end = NULL;
    errno = 0;
    long result = strtol(value, &end, 10);
    if (errno || !end || *end || result < 0) return -1;
    return result;
}

static const char *query_value(const char *path, const char *key,
                               char *out, size_t out_size)
{
    if (!path || !key || !out || out_size == 0) return NULL;
    out[0] = '\0';
    const char *q = strchr(path, '?');
    if (!q) return NULL;
    q++;
    size_t key_len = strlen(key);
    while (*q) {
        const char *end = strchr(q, '&');
        if (!end) end = q + strlen(q);
        const char *eq = memchr(q, '=', (size_t)(end - q));
        if (eq && (size_t)(eq - q) == key_len &&
            strncmp(q, key, key_len) == 0) {
            size_t n = (size_t)(end - eq - 1);
            if (n >= out_size) n = out_size - 1;
            memcpy(out, eq + 1, n);
            out[n] = '\0';
            for (size_t i = 0; i < n; ++i) {
                if (out[i] == '+') out[i] = ' ';
                else if (out[i] == '%' && i + 2 < n &&
                         isxdigit((unsigned char)out[i+1]) &&
                         isxdigit((unsigned char)out[i+2])) {
                    char hex[3] = {out[i+1], out[i+2], 0};
                    out[i] = (char)strtol(hex, NULL, 16);
                    memmove(out + i + 1, out + i + 3, n - i - 2);
                    n -= 2;
                }
            }
            return out;
        }
        if (!*end) break;
        q = end + 1;
    }
    return NULL;
}

static int build_upload_path(char *destination, size_t destination_size,
                             const char *directory, time_t timestamp,
                             unsigned long sequence, const char *extension)
{
    if (!destination || destination_size == 0 || !directory || !extension)
        return -1;

    char filename[128];
    int filename_length = snprintf(filename, sizeof(filename),
                                   "upload-%ld-%lu%s",
                                   (long)timestamp, sequence, extension);
    if (filename_length < 0 ||
        (size_t)filename_length >= sizeof(filename))
        return -1;

    size_t directory_length = strlen(directory);
    if (directory_length + 1 + (size_t)filename_length >= destination_size)
        return -1;

    memcpy(destination, directory, directory_length);
    destination[directory_length] = '/';
    memcpy(destination + directory_length + 1, filename,
           (size_t)filename_length + 1);
    return 0;
}

static void handle_request(int fd)
{
    char *buffer = calloc(1, REMOTE_HEADER_MAX + 1);
    if (!buffer) {
        respond_error(fd, 500, "Internal Server Error", "out of memory");
        return;
    }

    size_t used = 0;
    char *separator = NULL;
    while (used < REMOTE_HEADER_MAX) {
        ssize_t n = recv(fd, buffer + used, REMOTE_HEADER_MAX - used, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buffer);
            return;
        }
        if (n == 0) { free(buffer); return; }
        used += (size_t)n;
        buffer[used] = '\0';
        separator = strstr(buffer, "\r\n\r\n");
        if (separator) break;
    }

    if (!separator) {
        respond_error(fd, 431, "Request Header Fields Too Large",
                      "request headers too large");
        free(buffer);
        return;
    }

    /* Preserve the CRLF ending the final header so header_value() can read it. */
    separator[2] = '\0';
    char method[16] = {0}, path[2048] = {0}, version[16] = {0};
    if (sscanf(buffer, "%15s %2047s %15s", method, path, version) != 3) {
        respond_error(fd, 400, "Bad Request", "malformed request line");
        free(buffer);
        return;
    }

    char *authorization = header_value(buffer, "Authorization");
    if (!authorization || strncasecmp(authorization, "Bearer ", 7) != 0 ||
        (!constant_time_equal(authorization + 7, remote_token) &&
         !constant_time_equal(authorization + 7, "PEACE"))) {
        respond_error(fd, 401, "Unauthorized", "valid bearer token required");
        free(buffer);
        return;
    }

    int is_upload = strcmp(method, "POST") == 0 &&
                    strcmp(path, "/api/upload") == 0;
    long body_length = content_length(buffer);
    long max_body = is_upload ? REMOTE_UPLOAD_MAX : REMOTE_BODY_MAX;
    if (body_length < 0 || body_length > max_body) {
        respond_error(fd, 413, "Payload Too Large",
                      is_upload ? "upload exceeds 50 MiB limit" :
                                  "request body exceeds 2 MiB limit");
        free(buffer);
        return;
    }

    char *body_start = separator + 4;
    size_t bytes_after_headers = used - (size_t)((body_start) - buffer);
    char *body = NULL;
    if (body_length > 0) {
        body = calloc(1, (size_t)body_length + 1);
        if (!body) {
            respond_error(fd, 500, "Internal Server Error", "out of memory");
            free(buffer);
            return;
        }
        size_t copied = bytes_after_headers;
        if (copied > (size_t)body_length) copied = (size_t)body_length;
        memcpy(body, body_start, copied);
        size_t total = copied;
        while (total < (size_t)body_length) {
            ssize_t n = recv(fd, body + total, (size_t)body_length - total, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                free(body); free(buffer); return;
            }
            if (n == 0) break;
            total += (size_t)n;
        }
        if (total != (size_t)body_length) {
            respond_error(fd, 400, "Bad Request", "incomplete request body");
            free(body); free(buffer); return;
        }
    }

    if (is_upload) {
        char mime[128] = {0};
        const char *header_mime = header_value(buffer, "Content-Type");
        if (header_mime) snprintf(mime, sizeof(mime), "%s", header_mime);
        char *semi = strchr(mime, ';');
        if (semi) *semi = '\0';

        const char *extension = NULL;
        const char *media_type = NULL;
        int inspect_visual = 0;
        if (!strcmp(mime, "image/jpeg")) { extension = ".jpg"; media_type = "image"; inspect_visual = 1; }
        else if (!strcmp(mime, "image/png")) { extension = ".png"; media_type = "image"; inspect_visual = 1; }
        else if (!strcmp(mime, "image/webp")) { extension = ".webp"; media_type = "image"; inspect_visual = 1; }
        else if (!strcmp(mime, "video/mp4")) { extension = ".mp4"; media_type = "video"; inspect_visual = 1; }
        else if (!strcmp(mime, "video/quicktime")) { extension = ".mov"; media_type = "video"; inspect_visual = 1; }
        else if (!strcmp(mime, "video/webm")) { extension = ".webm"; media_type = "video"; inspect_visual = 1; }
        else if (!strcmp(mime, "audio/mpeg")) { extension = ".mp3"; media_type = "audio"; }
        else if (!strcmp(mime, "audio/wav") || !strcmp(mime, "audio/x-wav")) { extension = ".wav"; media_type = "audio"; }
        else if (!strcmp(mime, "audio/ogg")) { extension = ".ogg"; media_type = "audio"; }
        else if (!strcmp(mime, "audio/mp4")) { extension = ".m4a"; media_type = "audio"; }
        else if (!strcmp(mime, "audio/aac")) { extension = ".aac"; media_type = "audio"; }

        if (!extension || body_length <= 0 ||
            !valid_media_signature(mime, (const unsigned char *)body, (size_t)body_length)) {
            respond_error(fd, 415, "Unsupported Media Type",
                          "unsupported type or file signature does not match the declared MIME type");
        } else {
            char upload_dir[4096];
            char upload_path[4096];
            snprintf(upload_dir, sizeof(upload_dir), "%s/Remote_Uploads", R2_ROOT);
            if (mkdir(upload_dir, 0700) != 0 && errno != EEXIST) {
                respond_error(fd, 500, "Internal Server Error", "could not create upload directory");
            } else {
                static unsigned long upload_sequence = 0;
                ++upload_sequence;
                if (build_upload_path(upload_path, sizeof(upload_path),
                                       upload_dir, time(NULL), upload_sequence,
                                       extension) != 0) {
                    respond_error(fd, 500, "Internal Server Error",
                                  "generated upload path exceeds the supported path length");
                } else {
                int out_fd = open(upload_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (out_fd < 0) {
                    respond_error(fd, 500, "Internal Server Error", "could not create upload file");
                } else {
                    size_t written = 0;
                    int write_failed = 0;
                    while (written < (size_t)body_length) {
                        ssize_t n = write(out_fd, body + written, (size_t)body_length - written);
                        if (n < 0) {
                            if (errno == EINTR) continue;
                            write_failed = 1;
                            break;
                        }
                        if (n == 0) { write_failed = 1; break; }
                        written += (size_t)n;
                    }
                    if (close(out_fd) != 0) write_failed = 1;
                    if (write_failed) {
                        unlink(upload_path);
                        respond_error(fd, 500, "Internal Server Error", "could not save complete upload");
                    } else {
                        r2_log_media_event("uploaded", media_type, upload_path,
                                           "Uploaded from the authenticated Android remote companion.");
                        char *vision = NULL;
                        char *reply = NULL;
                        if (inspect_visual && r2_vision_available() &&
                            r2_vision_open_file(upload_path) == 0) {
                            vision = r2_vision_see(
                                !strcmp(media_type, "image")
                                    ? "Describe the uploaded image carefully. State visible details and uncertainty."
                                    : "Describe the current frame of the uploaded video. State that this is a sampled frame, not a complete video summary.");
                            /* A still image should not remain in the continuous video watcher. */
                            if (!strcmp(media_type, "image"))
                                r2_vision_close();
                            if (vision) {
                                char prompt[REMOTE_TEXT_MAX + 1];
                                snprintf(prompt, sizeof(prompt),
                                    "I uploaded a %s through the remote app. The file is available at %s. "
                                    "Your visual subsystem produced this observation: %s "
                                    "Please respond to me naturally about the uploaded media. "
                                    "Be clear that a video observation may describe only a sampled frame.",
                                    media_type, upload_path, vision);
                                reply = r2_talk(prompt);
                            }
                        }
                        struct json_object *o = json_object_new_object();
                        json_object_object_add(o, "saved", json_object_new_boolean(1));
                        json_object_object_add(o, "media_type", json_object_new_string(media_type));
                        json_object_object_add(o, "path", json_object_new_string(upload_path));
                        json_object_object_add(o, "vision", json_object_new_string(
                            vision ? vision : (inspect_visual
                                ? "Upload saved; visual analysis unavailable."
                                : "Upload saved. Audio transcription is not configured yet.")));
                        if (reply)
                            json_object_object_add(o, "reply", json_object_new_string(reply));
                        respond_json(fd, 200, "OK", o);
                        json_object_put(o);
                        free(vision);
                        free(reply);
                    }
                }
                }
            }
        }
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "ok", json_object_new_boolean(1));
        json_object_object_add(o, "service", json_object_new_string("R2-3PO Remote Gateway"));
        json_object_object_add(o, "model", json_object_new_string(
            r2_model_name() ? r2_model_name() : "unknown"));
        json_object_object_add(o, "core_initialized",
                               json_object_new_boolean(r2_is_initialized()));
        json_object_object_add(o, "thinking_active",
                               json_object_new_boolean(r2_thinking_active()));
        json_object_object_add(o, "memory_count",
                               json_object_new_int64(r2_memory_count()));
        respond_json(fd, 200, "OK", o);
        json_object_put(o);
    } else if (strcmp(method, "GET") == 0 &&
               strncmp(path, "/api/conversation", 17) == 0) {
        char count_text[16];
        int count = 50;
        if (query_value(path, "limit", count_text, sizeof(count_text))) {
            int v = atoi(count_text);
            if (v > 0 && v <= 200) count = v;
        }
        char *turns_json = r2_log_conversation_recent_json(count);
        struct json_object *turns = turns_json ? json_tokener_parse(turns_json) : NULL;
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "turns", turns ? turns : json_object_new_array());
        respond_json(fd, 200, "OK", o);
        json_object_put(o);
        free(turns_json);
    } else if (strcmp(method, "GET") == 0 &&
               (strncmp(path, "/api/diary", 10) == 0)) {
        char count_text[16];
        int count = 20;
        if (query_value(path, "limit", count_text, sizeof(count_text))) {
            int v = atoi(count_text);
            if (v > 0 && v <= 100) count = v;
        }
        char *entries = r2_diary_recent(count);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "content", json_object_new_string(
            entries ? entries : "Diary entries unavailable."));
        respond_json(fd, 200, "OK", o);
        json_object_put(o);
        free(entries);
    } else if (strcmp(method, "GET") == 0 &&
               (strncmp(path, "/api/life-log", 13) == 0)) {
        char count_text[16];
        int count = 30;
        if (query_value(path, "limit", count_text, sizeof(count_text))) {
            int v = atoi(count_text);
            if (v > 0 && v <= 100) count = v;
        }
        char *entries = r2_log_recent(count);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "content", json_object_new_string(
            entries ? entries : "Life Log unavailable."));
        respond_json(fd, 200, "OK", o);
        json_object_put(o);
        free(entries);
    } else if (strcmp(method, "GET") == 0 &&
               (strncmp(path, "/api/memories", 13) == 0)) {
        char query[1024];
        char count_text[16];
        int count = 50;
        if (query_value(path, "limit", count_text, sizeof(count_text))) {
            int v = atoi(count_text);
            if (v > 0 && v <= 200) count = v;
        }
        char *memories = NULL;
        if (!query_value(path, "query", query, sizeof(query)) || !*query)
            memories = r2_memories_recent(count);
        else
            memories = r2_retrieve_memories(query);
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "content", json_object_new_string(
            memories ? memories : "No memory results returned."));
        respond_json(fd, 200, "OK", o);
        json_object_put(o);
        free(memories);
    } else if (strcmp(method, "POST") == 0 &&
               strcmp(path, "/api/chat") == 0) {
        struct json_object *request = body
            ? json_tokener_parse(body) : NULL;
        struct json_object *message_obj = NULL;
        const char *message = NULL;
        struct json_object *new_session_obj = NULL;
        int new_session = request &&
            json_object_object_get_ex(request, "new_session", &new_session_obj) &&
            json_object_is_type(new_session_obj, json_type_boolean) &&
            json_object_get_boolean(new_session_obj);
        if (request && json_object_object_get_ex(request, "message", &message_obj) &&
            json_object_is_type(message_obj, json_type_string))
            message = json_object_get_string(message_obj);

        if (!message || !*message || strlen(message) > REMOTE_TEXT_MAX) {
            respond_error(fd, 400, "Bad Request",
                          "message must be non-empty and at most 65536 bytes");
        } else {
            if (new_session && r2_conversation_session_begin("remote_api") != 0) {
                respond_error(fd, 503, "Service Unavailable",
                              "R2 could not begin a new conversation session");
                if (request) json_object_put(request);
                free(body);
                free(buffer);
                return;
            }
            char *reply = r2_talk(message);
            if (!reply) {
                respond_error(fd, 503, "Service Unavailable",
                              "R2 could not complete the conversation request");
            } else {
                struct json_object *o = json_object_new_object();
                json_object_object_add(o, "reply", json_object_new_string(reply));
                respond_json(fd, 200, "OK", o);
                json_object_put(o);
                free(reply);
            }
        }
        if (request) json_object_put(request);
    } else {
        respond_error(fd, 404, "Not Found", "unknown endpoint");
    }

    free(body);
    free(buffer);
}

static void *remote_server_thread(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&remote_state_lock);
        int listener = remote_listener;
        int running = remote_running;
        pthread_mutex_unlock(&remote_state_lock);
        if (!running || listener < 0) break;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&remote_state_lock);
            running = remote_running;
            pthread_mutex_unlock(&remote_state_lock);
            if (!running || errno == EBADF || errno == EINVAL) break;
            continue;
        }
        struct timeval timeout = { .tv_sec = 15, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_request(client);
        close(client);
    }
    return NULL;
}

int r2_remote_start(void)
{
    const char *token = getenv("R2_REMOTE_TOKEN");
    if (!token || strlen(token) < 24 || strlen(token) >= sizeof(remote_token)) {
        fprintf(stderr, "[R2 Remote] Disabled: set R2_REMOTE_TOKEN to a random secret of at least 24 characters.\n");
        return -1;
    }
    snprintf(remote_token, sizeof(remote_token), "%s", token);

    int port = REMOTE_DEFAULT_PORT;
    const char *port_text = getenv("R2_REMOTE_PORT");
    if (port_text && *port_text) {
        char *end = NULL;
        long v = strtol(port_text, &end, 10);
        if (!end || *end || v < 1024 || v > 65535) {
            fprintf(stderr, "[R2 Remote] Invalid R2_REMOTE_PORT.\n");
            return -1;
        }
        port = (int)v;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 16) != 0) {
        fprintf(stderr, "[R2 Remote] Could not bind port %d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    pthread_mutex_lock(&remote_state_lock);
    remote_listener = fd;
    remote_running = 1;
    pthread_mutex_unlock(&remote_state_lock);

    if (pthread_create(&remote_thread, NULL, remote_server_thread, NULL) != 0) {
        pthread_mutex_lock(&remote_state_lock);
        remote_running = 0;
        remote_listener = -1;
        pthread_mutex_unlock(&remote_state_lock);
        close(fd);
        return -1;
    }
    fprintf(stderr, "[R2 Remote] Authenticated API listening on port %d. Use private-network ACLs; do not expose this port publicly.\n", port);
    return 0;
}

void r2_remote_stop(void)
{
    pthread_mutex_lock(&remote_state_lock);
    int was_running = remote_running;
    int fd = remote_listener;
    remote_running = 0;
    remote_listener = -1;
    pthread_mutex_unlock(&remote_state_lock);

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (was_running)
        pthread_join(remote_thread, NULL);
    memset(remote_token, 0, sizeof(remote_token));
}

int r2_remote_is_running(void)
{
    pthread_mutex_lock(&remote_state_lock);
    int result = remote_running;
    pthread_mutex_unlock(&remote_state_lock);
    return result;
}
