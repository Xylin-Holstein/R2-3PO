#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Visual.h"
#include "Log.h"
#include "r2.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R2_VISION_URL "http://127.0.0.1:11434/api/chat"
#define R2_VISION_DEFAULT_MODEL R2_OLLAMA_MODEL
#define R2_VISION_MAX_IMAGE_BYTES (20U * 1024U * 1024U)
#define R2_VISION_MAX_RESPONSE (1024U * 1024U)

static sqlite3 *visual_db = NULL;
static pthread_mutex_t visual_lock = PTHREAD_MUTEX_INITIALIZER;
static int visual_initialized = 0;
static char visual_directory[PATH_MAX];
static char visual_model[256];

struct response_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

static size_t response_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct response_buffer *b = userdata;
    if (!b || (size && nmemb > SIZE_MAX / size)) return 0;
    size_t amount = size * nmemb;
    if (amount > R2_VISION_MAX_RESPONSE - b->length) return 0;
    size_t needed = b->length + amount + 1;
    if (needed > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 4096;
        while (cap < needed) {
            if (cap > R2_VISION_MAX_RESPONSE / 2) {
                cap = R2_VISION_MAX_RESPONSE + 1;
                break;
            }
            cap *= 2;
        }
        char *next = realloc(b->data, cap);
        if (!next) return 0;
        b->data = next;
        b->capacity = cap;
    }
    memcpy(b->data + b->length, ptr, amount);
    b->length += amount;
    b->data[b->length] = '\0';
    return amount;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0750) == 0 || errno == EEXIST) return 0;
    return -1;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, p + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static unsigned char *read_file(const char *path, size_t *length)
{
    if (!path || !length) return NULL;
    *length = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > R2_VISION_MAX_IMAGE_BYTES) {
        close(fd);
        return NULL;
    }
    size_t cap = (size_t)st.st_size;
    unsigned char *data = malloc(cap);
    if (!data) { close(fd); return NULL; }
    size_t done = 0;
    while (done < cap) {
        ssize_t n = read(fd, data + done, cap - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); close(fd); return NULL; }
        done += (size_t)n;
    }
    close(fd);
    *length = cap;
    return data;
}

static char *base64_encode(const unsigned char *data, size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || !length || length > (SIZE_MAX - 4) / 4 * 3) return NULL;
    size_t outlen = ((length + 2) / 3) * 4;
    char *out = malloc(outlen + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < length) {
        size_t remaining = length - i;
        uint32_t a = data[i++];
        uint32_t b = remaining > 1 ? data[i++] : 0;
        uint32_t c = remaining > 2 ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = alphabet[(triple >> 18) & 63];
        out[j++] = alphabet[(triple >> 12) & 63];
        out[j++] = remaining > 1 ? alphabet[(triple >> 6) & 63] : '=';
        out[j++] = remaining > 2 ? alphabet[triple & 63] : '=';
    }
    out[outlen] = '\0';
    return out;
}

static unsigned char *encode_jpeg(const R2VisionFrame *frame, size_t *jpeg_length)
{
    if (!frame || !frame->data || !jpeg_length ||
        !frame->format.width || !frame->format.height ||
        frame->format.pixel_format != R2_PIXEL_RGB24)
        return NULL;

    uint64_t width = (uint64_t)frame->format.width;
    uint64_t height = (uint64_t)frame->format.height;
    if (height == 0 || width > UINT64_MAX / height / 3U)
        return NULL;
    uint64_t expected = width * height * 3U;
    if (expected != frame->size || expected > 100U * 1024U * 1024U)
        return NULL;

    char ppm_template[] = "/tmp/r2-visual-XXXXXX.ppm";
    char jpg_template[] = "/tmp/r2-visual-XXXXXX.jpg";
    int ppm_fd = mkstemps(ppm_template, 4);
    if (ppm_fd < 0) return NULL;
    int jpg_fd = mkstemps(jpg_template, 4);
    if (jpg_fd < 0) { close(ppm_fd); unlink(ppm_template); return NULL; }
    if (close(jpg_fd) != 0) {
        close(ppm_fd);
        unlink(ppm_template);
        unlink(jpg_template);
        return NULL;
    }
    /* Keep the unique file created by mkstemps; ffmpeg's -y overwrites it. */
    
    char header[128];
    int header_len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                              frame->format.width, frame->format.height);
    int ok = header_len > 0 && (size_t)header_len < sizeof(header) &&
             write_all(ppm_fd, header, (size_t)header_len) == 0 &&
             write_all(ppm_fd, frame->data, frame->size) == 0;
    if (close(ppm_fd) != 0) ok = 0;
    if (!ok) { unlink(ppm_template); return NULL; }

    pid_t pid = fork();
    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-y", "-i", ppm_template, "-frames:v", "1", "-q:v", "5",
               "-f", "image2", jpg_template, (char *)NULL);
        _exit(127);
    }

    int status = -1;
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    unlink(ppm_template);
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(jpg_template);
        return NULL;
    }

    unsigned char *jpeg = read_file(jpg_template, jpeg_length);
    unlink(jpg_template);
    return jpeg;
}

static int vision_progress(void *userdata,
                            curl_off_t download_total,
                            curl_off_t download_now,
                            curl_off_t upload_total,
                            curl_off_t upload_now)
{
    (void)userdata;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return r2_ollama_vision_request_should_abort() ? 1 : 0;
}

static char *call_vision_model(const char *image_base64, const char *question,
                               const char *model)
{
    if (!image_base64 || !model || !*model) return NULL;
    const char *asked = question && *question ? question :
        "Describe what is visibly present. Separate direct observations from uncertain interpretations. "
        "Read visible text when possible, identify objects and spatial relationships, and avoid guessing "
        "identity, intent, or emotion without visible evidence.";

    char *system_prompt = strdup(
        "You are the visual perception system of R2-3PO, a local robot. "
        "Analyze only the supplied image. Be concrete and evidence-based. "
        "Distinguish what is directly visible from inference and uncertainty. "
        "Do not claim motion or events from a single frame. Do not identify private people. "
        "Return a concise visual observation for R2's conversation to use as sensory evidence, not as a user-facing answer."
    );
    json_object *root = json_object_new_object();
    json_object *messages = json_object_new_array();
    json_object *sys = json_object_new_object();
    json_object *user = json_object_new_object();
    json_object *images = json_object_new_array();
    json_object *model_options = json_object_new_object();
    if (!root || !messages || !sys || !user || !images || !model_options || !system_prompt) {
        if (root) json_object_put(root);
        if (messages) json_object_put(messages);
        if (sys) json_object_put(sys);
        if (user) json_object_put(user);
        if (images) json_object_put(images);
        if (model_options) json_object_put(model_options);
        free(system_prompt);
        return NULL;
    }
    json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "stream", json_object_new_boolean(0));
    json_object_object_add(root, "keep_alive", json_object_new_string("10m"));
    json_object_object_add(model_options, "num_ctx", json_object_new_int(R2_OLLAMA_NUM_CTX));
    json_object_object_add(model_options, "num_batch", json_object_new_int(R2_OLLAMA_NUM_BATCH));
    json_object_object_add(model_options, "num_predict", json_object_new_int(256));
    json_object_object_add(root, "options", model_options);
    json_object_object_add(sys, "role", json_object_new_string("system"));
    json_object_object_add(sys, "content", json_object_new_string(system_prompt));
    json_object_object_add(user, "role", json_object_new_string("user"));
    json_object_object_add(user, "content", json_object_new_string(asked));
    json_object_array_add(images, json_object_new_string(image_base64));
    json_object_object_add(user, "images", images);
    json_object_array_add(messages, sys);
    json_object_array_add(messages, user);
    json_object_object_add(root, "messages", messages);
    free(system_prompt);

    const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    CURL *curl = curl_easy_init();
    struct response_buffer response = {0};
    struct curl_slist *headers = NULL;
    char *answer = NULL;
    if (curl) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, R2_VISION_URL);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, vision_progress);
        if (!r2_ollama_vision_request_begin()) {
            r2_log_sensory("vision_request_deferred",
                           "R2 deferred visual inference because the shared Ollama model is busy or a foreground turn is waiting.",
                           "The captured frame remains unreported rather than blocking ordinary conversation.",
                           model);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            free(response.data);
            json_object_put(root);
            return NULL;
        }
        CURLcode cc = curl_easy_perform(curl);
        int yielded_to_foreground =
            cc == CURLE_ABORTED_BY_CALLBACK &&
            r2_ollama_vision_request_should_abort();
        r2_ollama_vision_request_end();
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        if (cc == CURLE_OK && http_status >= 200 && http_status < 300 &&
            response.data) {
            json_object *parsed = json_tokener_parse(response.data);
            json_object *message = NULL, *content = NULL;
            if (parsed && json_object_object_get_ex(parsed, "message", &message) &&
                json_object_object_get_ex(message, "content", &content) &&
                json_object_is_type(content, json_type_string)) {
                const char *text = json_object_get_string(content);
                if (text && *text) answer = strdup(text);
            }
            if (parsed) json_object_put(parsed);
        } else if (yielded_to_foreground) {
            r2_log_sensory("vision_request_yielded",
                           "R2's visual inference yielded to an ordinary conversation request.",
                           "The incomplete visual result was discarded and will not be used as an answer.",
                           model);
        } else {
            char failure[1200];
            snprintf(failure, sizeof(failure),
                     "curl=%s; HTTP=%ld; response_bytes=%zu; model=%s",
                     curl_easy_strerror(cc), http_status, response.length, model);
            r2_log_sensory("vision_model_request_failed",
                           "R2's vision model request failed.",
                           response.data ? response.data : failure,
                           model);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    free(response.data);
    json_object_put(root);
    return answer;
}

int r2_visual_init(const char *database_path, const char *library_directory)
{
    if (!database_path || !*database_path || !library_directory ||
        !*library_directory || strlen(library_directory) >= sizeof(visual_directory))
        return -1;
    pthread_mutex_lock(&visual_lock);
    if (visual_initialized) { pthread_mutex_unlock(&visual_lock); return 0; }
    snprintf(visual_directory, sizeof(visual_directory), "%s", library_directory);
    const char *configured = getenv("R2_VISION_MODEL");
    if (configured && *configured &&
        strcmp(configured, R2_VISION_DEFAULT_MODEL) != 0) {
        fprintf(stderr,
                "[R2 Vision] Ignoring R2_VISION_MODEL=%s; this build uses the unified model %s.\n",
                configured, R2_VISION_DEFAULT_MODEL);
    }
    snprintf(visual_model, sizeof(visual_model), "%s", R2_VISION_DEFAULT_MODEL);
    if (mkdir_one(visual_directory) != 0) {
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    if (sqlite3_open(database_path, &visual_db) != SQLITE_OK) {
        if (visual_db) sqlite3_close(visual_db);
        visual_db = NULL;
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    sqlite3_busy_timeout(visual_db, 5000);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_visual_experiences ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_utc TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "image_path TEXT NOT NULL,"
        "model TEXT NOT NULL,"
        "prompt TEXT,"
        "description TEXT NOT NULL,"
        "width INTEGER NOT NULL,"
        "height INTEGER NOT NULL,"
        "frame_number INTEGER NOT NULL,"
        "frame_timestamp INTEGER NOT NULL,"
        "life_log_event_id INTEGER,"
        "UNIQUE(image_path));"
        "CREATE INDEX IF NOT EXISTS r2_visual_experiences_created_idx "
        "ON r2_visual_experiences(created_utc);";
    char *err = NULL;
    if (sqlite3_exec(visual_db, schema, NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(visual_db);
        visual_db = NULL;
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    visual_initialized = 1;
    pthread_mutex_unlock(&visual_lock);
    r2_log_sensory("visual_library_initialized",
                   "R2 Visual Experience Library initialized.",
                   visual_directory, visual_model);
    return 0;
}

void r2_visual_shutdown(void)
{
    pthread_mutex_lock(&visual_lock);
    if (visual_db) sqlite3_close(visual_db);
    visual_db = NULL;
    visual_initialized = 0;
    pthread_mutex_unlock(&visual_lock);
}

int r2_visual_set_model(const char *model)
{
    if (!model || !*model || strlen(model) >= sizeof(visual_model))
        return -1;
    for (const unsigned char *p = (const unsigned char *)model; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == ':' || *p == '_' ||
               *p == '-' || *p == '.' || *p == '/'))
            return -1;
    }
    if (strcmp(model, R2_VISION_DEFAULT_MODEL) != 0) {
        fprintf(stderr,
                "[R2 Vision] Rejected model %s; conversation and vision must share %s.\n",
                model, R2_VISION_DEFAULT_MODEL);
        return -1;
    }
    pthread_mutex_lock(&visual_lock);
    snprintf(visual_model, sizeof(visual_model), "%s", R2_VISION_DEFAULT_MODEL);
    pthread_mutex_unlock(&visual_lock);
    r2_log_sensory("vision_model_selected",
                   "R2's visual perception model was selected.",
                   model, "Visual.c");
    return 0;
}

int r2_visual_is_initialized(void)
{
    pthread_mutex_lock(&visual_lock);
    int result = visual_initialized;
    pthread_mutex_unlock(&visual_lock);
    return result;
}

const char *r2_visual_model_name(void)
{
    static _Thread_local char model_snapshot[sizeof(visual_model)];
    pthread_mutex_lock(&visual_lock);
    snprintf(model_snapshot, sizeof(model_snapshot), "%s",
             visual_model[0] ? visual_model : R2_VISION_DEFAULT_MODEL);
    pthread_mutex_unlock(&visual_lock);
    return model_snapshot;
}

char *r2_visual_analyze_frame(const R2VisionFrame *frame,
                              const char *source,
                              const char *question)
{
    if (!frame || !frame->data) return NULL;

    char model_snapshot[sizeof(visual_model)];
    char directory_snapshot[sizeof(visual_directory)];
    pthread_mutex_lock(&visual_lock);
    if (!visual_initialized || !visual_db) {
        pthread_mutex_unlock(&visual_lock);
        return NULL;
    }
    snprintf(model_snapshot, sizeof(model_snapshot), "%s", visual_model);
    snprintf(directory_snapshot, sizeof(directory_snapshot), "%s", visual_directory);
    pthread_mutex_unlock(&visual_lock);

    size_t jpeg_length = 0;
    unsigned char *jpeg = encode_jpeg(frame, &jpeg_length);
    if (!jpeg) {
        r2_log_sensory("visual_encoding_failed",
                       "R2 could not encode a captured frame for vision inference.",
                       "FFmpeg conversion from RGB24 to JPEG failed.", source);
        return NULL;
    }
    char *base64 = base64_encode(jpeg, jpeg_length);
    if (!base64) { free(jpeg); return NULL; }
    char *description = call_vision_model(base64, question, model_snapshot);
    free(base64);
    if (!description) { free(jpeg); return NULL; }

    char image_path[PATH_MAX];
    char filename[128];
    int filename_length = snprintf(filename, sizeof(filename),
             "visual_%llu_%llu.jpg",
             (unsigned long long)frame->timestamp,
             (unsigned long long)frame->frame_number);
    size_t directory_length = strlen(directory_snapshot);
    size_t separator_length =
        directory_length > 0 && directory_snapshot[directory_length - 1] == '/' ? 0 : 1;
    if (filename_length < 0 || (size_t)filename_length >= sizeof(filename) ||
        directory_length + separator_length + (size_t)filename_length + 1 >
            sizeof(image_path)) {
        r2_log_sensory("visual_image_archive_failed",
                       "R2 analyzed a frame but its archive path was too long.",
                       directory_snapshot, source ? source : "Eyes");
        free(jpeg);
        free(description);
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(image_path, directory_snapshot, directory_length);
    size_t path_offset = directory_length;
    if (separator_length)
        image_path[path_offset++] = '/';
    memcpy(image_path + path_offset, filename, (size_t)filename_length + 1);

    int image_fd = open(image_path, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (image_fd < 0) {
        int saved_errno = errno;
        r2_log_sensory("visual_image_archive_failed",
                       "R2 analyzed an image but could not create its JPEG archive.",
                       strerror(saved_errno), image_path);
        free(jpeg);
        free(description);
        return NULL; /* Never unlink a path that this call did not create. */
    }
    if (write_all(image_fd, jpeg, jpeg_length) != 0) {
        int saved_errno = errno;
        close(image_fd);
        unlink(image_path); /* This call created the file. */
        r2_log_sensory("visual_image_archive_failed",
                       "R2 analyzed an image but could not write its JPEG archive.",
                       strerror(saved_errno), image_path);
        free(jpeg);
        free(description);
        return NULL;
    }
    if (close(image_fd) != 0) {
        int saved_errno = errno;
        unlink(image_path);
        r2_log_sensory("visual_image_archive_failed",
                       "R2 analyzed an image but could not close its JPEG archive cleanly.",
                       strerror(saved_errno), image_path);
        free(jpeg);
        free(description);
        return NULL;
    }
    free(jpeg);

    time_t now = time(NULL);
    struct tm utc;
    char created[40] = "unknown";
    if (gmtime_r(&now, &utc))
        strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc);

    int64_t event_id = r2_log_sensory(
        "visual_experience",
        "R2's vision model interpreted a real captured frame.",
        description, source ? source : "Eyes");
    pthread_mutex_lock(&visual_lock);
    if (!visual_initialized || !visual_db) {
        pthread_mutex_unlock(&visual_lock);
        unlink(image_path);
        r2_log_sensory("visual_experience_store_failed",
                       "R2 analyzed a frame, but the visual library shut down before it could be indexed.",
                       "The JPEG archive was removed because no matching database record could be written.",
                       image_path);
        free(description);
        return NULL;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO r2_visual_experiences "
        "(created_utc,source,image_path,model,prompt,description,width,height,"
        "frame_number,frame_timestamp,life_log_event_id) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)";
    int rc = sqlite3_prepare_v2(visual_db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(st, 1, created, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, source ? source : "Eyes", -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, image_path, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 4, model_snapshot, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) {
            if (question && *question) rc = sqlite3_bind_text(st, 5, question, -1, SQLITE_TRANSIENT);
            else rc = sqlite3_bind_null(st, 5);
        }
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 6, description, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 7, (int)frame->format.width);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 8, (int)frame->format.height);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 9, (sqlite3_int64)frame->frame_number);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 10, (sqlite3_int64)frame->timestamp);
        if (rc == SQLITE_OK) {
            if (event_id > 0) rc = sqlite3_bind_int64(st, 11, (sqlite3_int64)event_id);
            else rc = sqlite3_bind_null(st, 11);
        }
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    char db_error[512] = "unknown SQLite error";
    if (rc != SQLITE_DONE)
        snprintf(db_error, sizeof(db_error), "%s", sqlite3_errmsg(visual_db));
    sqlite3_finalize(st);
    pthread_mutex_unlock(&visual_lock);
    if (rc != SQLITE_DONE) {
        unlink(image_path);
        r2_log_sensory("visual_experience_store_failed",
                       "R2 analyzed a frame but failed to save the library record.",
                       db_error, image_path);
        free(description);
        return NULL;
    }
    return description;
}

static char *visual_query(const char *sql, const char *query, int limit)
{
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    struct response_buffer b = {0};
    int failed = 0;
    pthread_mutex_lock(&visual_lock);
    if (!visual_initialized || !visual_db) {
        pthread_mutex_unlock(&visual_lock);
        return NULL;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(visual_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&visual_lock);
        return NULL;
    }

    if (query) {
        rc = sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK && strstr(sql, "lower(source)") != NULL) {
            rc = sqlite3_bind_text(st, 2, query, -1, SQLITE_TRANSIENT);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, limit);
        } else if (rc == SQLITE_OK) {
            rc = sqlite3_bind_int(st, 2, limit);
        }
    } else {
        rc = sqlite3_bind_int(st, 1, limit);
    }

    if (rc != SQLITE_OK) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&visual_lock);
        return NULL;
    }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *created = sqlite3_column_text(st, 0);
        const unsigned char *source = sqlite3_column_text(st, 1);
        const unsigned char *model = sqlite3_column_text(st, 2);
        const unsigned char *desc = sqlite3_column_text(st, 3);
        const unsigned char *path = sqlite3_column_text(st, 4);
        char line[8192];
        int n = snprintf(line, sizeof(line), "[%s] source=%s model=%s image=%s\n%s\n\n",
            created ? (const char *)created : "?",
            source ? (const char *)source : "?",
            model ? (const char *)model : "?",
            path ? (const char *)path : "?",
            desc ? (const char *)desc : "(no description)");
        if (n < 0) {
            failed = 1;
            break;
        }

        size_t add = (size_t)n;
        if (add >= sizeof(line)) add = sizeof(line) - 1;
        if (add > SIZE_MAX - b.length - 1) {
            failed = 1;
            break;
        }
        char *next = realloc(b.data, b.length + add + 1);
        if (!next) {
            failed = 1;
            break;
        }
        b.data = next;
        memcpy(b.data + b.length, line, add);
        b.length += add;
        b.data[b.length] = '\0';
    }

    if (rc != SQLITE_DONE)
        failed = 1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&visual_lock);

    if (failed) {
        free(b.data);
        return NULL;
    }
    if (!b.data)
        b.data = strdup("(No visual experiences found.)\n");
    return b.data;
}

char *r2_visual_recent(int limit)
{
    return visual_query(
        "SELECT created_utc,source,model,description,image_path "
        "FROM r2_visual_experiences ORDER BY id DESC LIMIT ?",
        NULL, limit);
}

char *r2_visual_search(const char *query, int limit)
{
    if (!query || !*query) return r2_visual_recent(limit);
    return visual_query(
        "SELECT created_utc,source,model,description,image_path "
        "FROM r2_visual_experiences WHERE instr(lower(description),lower(?))>0 "
        "OR instr(lower(source),lower(?))>0 ORDER BY id DESC LIMIT ?",
        query, limit);
}
