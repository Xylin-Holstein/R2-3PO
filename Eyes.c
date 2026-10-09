#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Eyes.h"
#include "Log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * R2-3PO Eyes
 *
 * IMPORTANT:
 *
 * R2's EYES are NOT limited to 640x480.
 *
 * 640x480 is the resolution used when R2 chooses to RECORD /
 * retain a visual moment.
 *
 * His visual input remains at the source's native resolution.
 *
 * Example:
 *
 *     1920x1080 movie
 *             |
 *             v
 *          R2 EYES
 *             |
 *       1920x1080 RGB
 *             |
 *       vision/cognition
 *             |
 *       "I want THAT"
 *             |
 *             v
 *        640x480 capture
 *
 * This means R2 never has visual information thrown away before
 * his vision system has an opportunity to examine it.
 *
 * Eyes itself does NOT:
 *
 *     - recognize faces
 *     - identify objects
 *     - read text
 *     - describe scenes
 *     - determine emotions
 *     - choose what R2 should look at
 *     - create memories
 *
 * Eyes only supplies the visual stream.
 */

/* Recording resolution */
#define R2_EYES_RECORD_WIDTH       640
#define R2_EYES_RECORD_HEIGHT      480
#define R2_EYES_RECORD_BPP         24
#define R2_EYES_RECORD_CHANNELS    3
#define R2_EYES_RECORD_SIZE \
    ((size_t)R2_EYES_RECORD_WIDTH * \
     (size_t)R2_EYES_RECORD_HEIGHT * \
     R2_EYES_RECORD_CHANNELS)

/* Maximum native frame size */
#define R2_EYES_MAX_FRAME_BYTES \
    ((size_t)7680 * (size_t)4320 * 3)

/* Sampling rate */
#define R2_EYES_FPS 10

struct R2Eyes
{
    FILE *stream;
    int open;
    int source_is_live;
    R2VisionOrigin origin;
    R2VideoFormat format;

    unsigned char *frame_buffer;
    size_t frame_size;

    unsigned char *record_buffer;
    size_t record_size;

    uint64_t frame_count;
    uint64_t timestamp;
    uint64_t last_log_timestamp;

    R2VisionEvent event;
    R2VisionFrame frame;

    char source_name[512];
};

static uint64_t r2_eyes_timestamp(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return ((uint64_t)ts.tv_sec * 1000ULL) +
           ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static char *r2_eyes_shell_quote(const char *input)
{
    if (!input)
        return NULL;

    size_t length = strlen(input);
    size_t max_size = (length * 4) + 3;

    char *output = malloc(max_size);

    if (!output)
        return NULL;

    char *p = output;

    *p++ = '\'';

    for (size_t i = 0; i < length; ++i)
    {
        if (input[i] == '\'')
        {
            memcpy(p, "'\\''", 4);
            p += 4;
        }
        else
        {
            *p++ = input[i];
        }
    }

    *p++ = '\'';
    *p = '\0';

    return output;
}

static int r2_eyes_probe_dimensions(
    const char *source,
    int live_device,
    uint32_t *width,
    uint32_t *height)
{
    if (!source || !width || !height)
        return -1;

    if (live_device)
    {
        *width = 640;
        *height = 480;

        return 0;
    }

    char *quoted_source =
        r2_eyes_shell_quote(source);

    if (!quoted_source)
        return -1;

    char command[4096];

    snprintf(
        command,
        sizeof(command),
        "ffprobe "
        "-v error "
        "-select_streams v:0 "
        "-show_entries stream=width,height "
        "-of csv=p=0:s=x "
        "%s",
        quoted_source
    );

    free(quoted_source);

    FILE *probe = popen(command, "r");

    if (!probe)
        return -1;

    unsigned int w = 0;
    unsigned int h = 0;

    int result =
        fscanf(probe, "%ux%u", &w, &h);

    int status = pclose(probe);

    if (result != 2 || status == -1)
        return -1;

    if (w == 0 || h == 0)
        return -1;

    size_t required =
        (size_t)w *
        (size_t)h *
        3;

    if (required > R2_EYES_MAX_FRAME_BYTES)
        return -1;

    *width = (uint32_t)w;
    *height = (uint32_t)h;

    return 0;
}

static int r2_eyes_allocate_native_buffer(
    R2Eyes *eyes,
    uint32_t width,
    uint32_t height)
{
    if (!eyes || width == 0 || height == 0)
        return -1;

    size_t frame_size =
        (size_t)width *
        (size_t)height *
        3;

    if (frame_size > R2_EYES_MAX_FRAME_BYTES)
        return -1;

    unsigned char *buffer =
        malloc(frame_size);

    if (!buffer)
        return -1;

    free(eyes->frame_buffer);

    eyes->frame_buffer = buffer;
    eyes->frame_size = frame_size;

    eyes->format.width = width;
    eyes->format.height = height;
    eyes->format.stride = width * 3;
    eyes->format.bits_per_pixel = 24;
    eyes->format.pixel_format = R2_PIXEL_RGB24;

    return 0;
}

static int r2_eyes_allocate_record_buffer(R2Eyes *eyes)
{
    if (!eyes)
        return -1;

    if (!eyes->record_buffer)
    {
        eyes->record_buffer =
            malloc(R2_EYES_RECORD_SIZE);

        if (!eyes->record_buffer)
            return -1;
    }

    eyes->record_size =
        R2_EYES_RECORD_SIZE;

    memset(
        eyes->record_buffer,
        0,
        eyes->record_size
    );

    return 0;
}

static int r2_eyes_read_exact(
    FILE *stream,
    unsigned char *buffer,
    size_t size)
{
    size_t total = 0;

    while (total < size)
    {
        size_t amount =
            fread(
                buffer + total,
                1,
                size - total,
                stream
            );

        if (amount > 0)
        {
            total += amount;
            continue;
        }

        if (feof(stream))
            return 0;

        if (ferror(stream))
            return -1;
    }

    return 1;
}

static void r2_eyes_reset_frame(R2Eyes *eyes)
{
    if (!eyes)
        return;

    memset(
        &eyes->frame,
        0,
        sizeof(eyes->frame)
    );

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;

    eyes->frame.format =
        eyes->format;

    eyes->frame.frame_number =
        eyes->frame_count;

    eyes->frame.timestamp =
        eyes->timestamp;
}

static void r2_eyes_reset_event(R2Eyes *eyes)
{
    if (!eyes)
        return;

    memset(
        &eyes->event,
        0,
        sizeof(eyes->event)
    );

    eyes->event.origin =
        eyes->origin;

    eyes->event.type =
        R2_VISUAL_UNKNOWN;

    snprintf(
        eyes->event.source_name,
        sizeof(eyes->event.source_name),
        "%s",
        eyes->source_name
    );

    eyes->event.format =
        eyes->format;

    eyes->event.timestamp =
        eyes->timestamp;

    eyes->event.frame_count =
        eyes->frame_count;
}

static FILE *r2_eyes_start_ffmpeg(
    const char *input,
    int live_device)
{
    if (!input)
        return NULL;

    char *quoted_input =
        r2_eyes_shell_quote(input);

    if (!quoted_input)
        return NULL;

    char command[4096];

    if (live_device)
    {
        snprintf(
            command,
            sizeof(command),
            "ffmpeg "
            "-nostdin "
            "-hide_banner "
            "-loglevel error "
            "-f v4l2 "
            "-framerate %d "
            "-video_size 640x480 "
            "-i %s "
            "-vf format=rgb24 "
            "-f rawvideo "
            "-pix_fmt rgb24 "
            "-r %d "
            "pipe:1",
            R2_EYES_FPS,
            quoted_input,
            R2_EYES_FPS
        );
    }
    else
    {
        snprintf(
            command,
            sizeof(command),
            "ffmpeg "
            "-nostdin "
            "-hide_banner "
            "-loglevel error "
            "-re "
            "-i %s "
            "-vf "
            "\"fps=%d,format=rgb24\" "
            "-f rawvideo "
            "-pix_fmt rgb24 "
            "pipe:1",
            quoted_input,
            R2_EYES_FPS
        );
    }

    free(quoted_input);

    return popen(command, "r");
}

/* ============================================================
   VLC SUPPORT
   ============================================================ */

/*
 * Find a visible VLC window through X11.
 *
 * Eyes captures the VLC window itself, NOT the entire desktop.
 */
static int r2_eyes_find_vlc_window(
    unsigned long *window_id)
{
    if (!window_id)
        return -1;

    FILE *pipe =
        popen(
            "xdotool search --onlyvisible "
            "--class '^vlc$' 2>/dev/null",
            "r"
        );

    if (!pipe)
        return -1;

    unsigned long id = 0;

    int result =
        fscanf(pipe, "%lu", &id);

    int status =
        pclose(pipe);

    if (result != 1 ||
        status == -1 ||
        id == 0)
    {
        return -1;
    }

    *window_id = id;

    return 0;
}

/*
 * Determine the current VLC window dimensions.
 *
 * These dimensions become R2's native visual input size
 * for the VLC source.
 */
static int r2_eyes_probe_vlc_dimensions(
    unsigned long window_id,
    uint32_t *width,
    uint32_t *height)
{
    if (!width ||
        !height ||
        window_id == 0)
    {
        return -1;
    }

    char command[256];

    snprintf(
        command,
        sizeof(command),
        "xdotool getwindowgeometry "
        "--shell %lu 2>/dev/null",
        window_id
    );

    FILE *pipe =
        popen(command, "r");

    if (!pipe)
        return -1;

    unsigned int w = 0;
    unsigned int h = 0;

    char line[128];

    while (fgets(
        line,
        sizeof(line),
        pipe))
    {
        unsigned int value;

        if (sscanf(
            line,
            "WIDTH=%u",
            &value) == 1)
        {
            w = value;
        }

        if (sscanf(
            line,
            "HEIGHT=%u",
            &value) == 1)
        {
            h = value;
        }
    }

    int status =
        pclose(pipe);

    if (status == -1 ||
        w == 0 ||
        h == 0)
    {
        return -1;
    }

    size_t required =
        (size_t)w *
        (size_t)h *
        3;

    if (required >
        R2_EYES_MAX_FRAME_BYTES)
    {
        return -1;
    }

    *width = (uint32_t)w;
    *height = (uint32_t)h;

    return 0;
}

/*
 * Start an FFmpeg X11 capture attached specifically
 * to the VLC window.
 */
static FILE *r2_eyes_start_vlc(
    unsigned long window_id)
{
    if (window_id == 0)
        return NULL;

    const char *display =
        getenv("DISPLAY");

    if (!display ||
        display[0] == '\0')
    {
        return NULL;
    }

    char *quoted_display =
        r2_eyes_shell_quote(display);

    if (!quoted_display)
        return NULL;

    char command[4096];

    snprintf(
        command,
        sizeof(command),
        "ffmpeg "
        "-nostdin "
        "-hide_banner "
        "-loglevel error "
        "-f x11grab "
        "-framerate %d "
        "-window_id %lu "
        "-draw_mouse 0 "
        "-i %s "
        "-vf \"format=rgb24\" "
        "-f rawvideo "
        "-pix_fmt rgb24 "
        "-r %d "
        "pipe:1",
        R2_EYES_FPS,
        window_id,
        quoted_display,
        R2_EYES_FPS
    );

    free(quoted_display);

    return popen(command, "r");
}

/* ============================================================
   INITIALIZATION
   ============================================================ */

int r2_eyes_init(R2Eyes **eyes)
{
    if (!eyes)
        return -1;

    *eyes = NULL;

    R2Eyes *instance =
        calloc(1, sizeof(R2Eyes));

    if (!instance)
        return -1;

    instance->format.width = 0;
    instance->format.height = 0;
    instance->format.stride = 0;
    instance->format.bits_per_pixel = 0;
    instance->format.pixel_format =
        R2_PIXEL_UNKNOWN;

    if (r2_eyes_allocate_record_buffer(
            instance) != 0)
    {
        free(instance);
        return -1;
    }

    instance->origin =
        R2_VISION_NONE;

    instance->frame_count = 0;
    instance->timestamp = 0;

    r2_eyes_reset_frame(instance);
    r2_eyes_reset_event(instance);

    *eyes = instance;

    return 0;
}

/* ============================================================
   CLOSE / SHUTDOWN
   ============================================================ */

void r2_eyes_close(R2Eyes *eyes)
{
    if (!eyes)
        return;

    if (eyes->stream)
    {
        pclose(eyes->stream);
        eyes->stream = NULL;
    }

    if (eyes->open) {
        char details[768];
        snprintf(details, sizeof(details),
                 "source=%s; frames_captured=%llu; resolution=%ux%u",
                 eyes->source_name[0] ? eyes->source_name : "(unknown)",
                 (unsigned long long)eyes->frame_count,
                 eyes->format.width, eyes->format.height);
        r2_log_sensory("vision_source_closed",
                       "R2 Eyes stopped receiving visual frames.",
                       details, "Eyes.c");
    }

    eyes->open = 0;
    eyes->source_is_live = 0;
    eyes->origin = R2_VISION_NONE;
    eyes->frame_count = 0;
    eyes->timestamp = 0;
    eyes->last_log_timestamp = 0;

    memset(
        eyes->source_name,
        0,
        sizeof(eyes->source_name)
    );

    memset(
        &eyes->event,
        0,
        sizeof(eyes->event)
    );

    memset(
        &eyes->frame,
        0,
        sizeof(eyes->frame)
    );

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;
}

void r2_eyes_shutdown(R2Eyes *eyes)
{
    if (!eyes)
        return;

    r2_eyes_close(eyes);

    free(eyes->frame_buffer);
    eyes->frame_buffer = NULL;

    free(eyes->record_buffer);
    eyes->record_buffer = NULL;

    free(eyes);
}

/* ============================================================
   GENERIC SOURCE OPENING
   ============================================================ */

static int r2_eyes_open_internal(
    R2Eyes *eyes,
    const char *source,
    R2VisionOrigin origin,
    int live_device)
{
    if (!eyes || !source)
        return -1;

    r2_eyes_close(eyes);

    uint32_t width = 0;
    uint32_t height = 0;

    if (r2_eyes_probe_dimensions(
            source,
            live_device,
            &width,
            &height) != 0)
    {
        r2_log_sensory("vision_source_open_failed",
                       "R2 Eyes could not determine the input dimensions.",
                       "The source could not be probed or had unsupported dimensions.",
                       source);
        return -1;
    }

    if (r2_eyes_allocate_native_buffer(
            eyes,
            width,
            height) != 0)
    {
        r2_log_sensory("vision_buffer_allocation_failed",
                       "R2 Eyes could not allocate a native frame buffer.",
                       "The source could not be opened because frame memory allocation failed.",
                       source);
        return -1;
    }

    if (r2_eyes_allocate_record_buffer(
            eyes) != 0)
    {
        return -1;
    }

    FILE *stream =
        r2_eyes_start_ffmpeg(
            source,
            live_device
        );

    if (!stream) {
        r2_log_sensory("vision_stream_open_failed",
                       "R2 Eyes could not start the FFmpeg capture stream.",
                       "FFmpeg did not provide a readable visual stream.",
                       source);
        return -1;
    }

    eyes->stream = stream;
    eyes->open = 1;
    eyes->source_is_live = live_device;
    eyes->origin = origin;
    eyes->frame_count = 0;
    eyes->timestamp = 0;

    strncpy(
        eyes->source_name,
        source,
        sizeof(eyes->source_name) - 1
    );

    eyes->source_name[
        sizeof(eyes->source_name) - 1
    ] = '\0';

    r2_eyes_reset_event(eyes);
    r2_eyes_reset_frame(eyes);

    char details[768];
    snprintf(details, sizeof(details),
             "origin=%s; resolution=%ux%u; pixel_format=RGB24; sampling_fps=%d",
             origin == R2_VISION_WORLD ? "world_camera" : "file",
             width, height, R2_EYES_FPS);
    r2_log_sensory("vision_source_opened",
                   "R2 Eyes opened a visual input source.",
                   details, eyes->source_name);

    return 0;
}

int r2_eyes_open_file(
    R2Eyes *eyes,
    const char *path)
{
    if (!eyes ||
        !path ||
        path[0] == '\0')
    {
        return -1;
    }

    return r2_eyes_open_internal(
        eyes,
        path,
        R2_VISION_FILE,
        0
    );
}

int r2_eyes_open_camera(
    R2Eyes *eyes)
{
    return r2_eyes_open_device(
        eyes,
        "/dev/video0"
    );
}

int r2_eyes_open_device(
    R2Eyes *eyes,
    const char *device)
{
    if (!eyes ||
        !device ||
        device[0] == '\0')
    {
        return -1;
    }

    return r2_eyes_open_internal(
        eyes,
        device,
        R2_VISION_WORLD,
        1
    );
}

int r2_eyes_open_source(
    R2Eyes *eyes,
    const char *source_name)
{
    if (!eyes ||
        !source_name ||
        source_name[0] == '\0')
    {
        return -1;
    }

    if (strncmp(
            source_name,
            "/dev/video",
            10
        ) == 0)
    {
        return r2_eyes_open_device(
            eyes,
            source_name
        );
    }

    return r2_eyes_open_file(
        eyes,
        source_name
    );
}

/* ============================================================
   VLC SOURCE
   ============================================================ */

int r2_eyes_open_vlc(
    R2Eyes *eyes)
{
    if (!eyes)
        return -1;

    r2_eyes_close(eyes);

    unsigned long window_id = 0;

    if (r2_eyes_find_vlc_window(
            &window_id) != 0)
    {
        r2_log_sensory("vlc_window_not_found",
                       "R2 Eyes could not find a visible VLC window.",
                       "VLC window capture requires a visible X11 VLC window and xdotool.",
                       "VLC");
        return -1;
    }

    uint32_t width = 0;
    uint32_t height = 0;

    if (r2_eyes_probe_vlc_dimensions(
            window_id,
            &width,
            &height) != 0)
    {
        return -1;
    }

    if (r2_eyes_allocate_native_buffer(
            eyes,
            width,
            height) != 0)
    {
        return -1;
    }

    if (r2_eyes_allocate_record_buffer(
            eyes) != 0)
    {
        return -1;
    }

    FILE *stream =
        r2_eyes_start_vlc(
            window_id
        );

    if (!stream)
        return -1;

    eyes->stream = stream;
    eyes->open = 1;

    /*
     * VLC is visually live, but the capture should close
     * when the VLC window disappears.
     */
    eyes->source_is_live = 0;

    eyes->origin =
        R2_VISION_VLC;

    /*
     * Preserve the visible VLC window title as source metadata. This is
     * useful for later history searches, but is not treated as proof of
     * the actual media contents.
     */
    char title_command[256];
    snprintf(title_command, sizeof(title_command),
             "xdotool getwindowname %lu 2>/dev/null", window_id);
    FILE *title_pipe = popen(title_command, "r");
    char window_title[sizeof(eyes->source_name)] = "VLC";
    if (title_pipe) {
        if (!fgets(window_title, sizeof(window_title), title_pipe))
            snprintf(window_title, sizeof(window_title), "VLC");
        pclose(title_pipe);
        size_t title_len = strlen(window_title);
        while (title_len > 0 &&
               (window_title[title_len - 1] == '\n' ||
                window_title[title_len - 1] == '\r'))
            window_title[--title_len] = '\0';
        if (title_len == 0)
            snprintf(window_title, sizeof(window_title), "VLC");
    }
    snprintf(eyes->source_name, sizeof(eyes->source_name), "%s", window_title);

    eyes->frame_count = 0;
    eyes->timestamp = 0;

    /* source_name was populated from the visible VLC window title above. */
    r2_eyes_reset_event(eyes);
    r2_eyes_reset_frame(eyes);

    r2_log_sensory("vision_source_opened",
                   "R2 Eyes began capturing the visible VLC window.",
                   "origin=VLC; frames are raw pixels until a vision model interprets them.",
                   eyes->source_name);

    return 0;
}

/* ============================================================
   FRAME CAPTURE
   ============================================================ */

int r2_eyes_capture(
    R2Eyes *eyes)
{
    if (!eyes ||
        !eyes->open ||
        !eyes->stream ||
        !eyes->frame_buffer ||
        eyes->frame_size == 0)
    {
        return -1;
    }

    int result =
        r2_eyes_read_exact(
            eyes->stream,
            eyes->frame_buffer,
            eyes->frame_size
        );

    if (result == 0)
    {
        if (!eyes->source_is_live)
            r2_eyes_close(eyes);

        return 0;
    }

    if (result < 0)
    {
        r2_log_sensory("visual_frame_read_failed",
                       "R2 Eyes encountered an error reading a visual frame.",
                       "The capture stream reported a read error.",
                       eyes->source_name);
        r2_eyes_close(eyes);
        return -1;
    }

    eyes->frame_count++;
    eyes->timestamp =
        r2_eyes_timestamp();

    eyes->event.origin =
        eyes->origin;

    eyes->event.type =
        R2_VISUAL_UNKNOWN;

    strncpy(
        eyes->event.source_name,
        eyes->source_name,
        sizeof(eyes->event.source_name) - 1
    );

    eyes->event.source_name[
        sizeof(eyes->event.source_name) - 1
    ] = '\0';

    eyes->event.format =
        eyes->format;

    eyes->event.timestamp =
        eyes->timestamp;

    eyes->event.frame_count =
        eyes->frame_count;

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;

    eyes->frame.format =
        eyes->format;

    eyes->frame.timestamp =
        eyes->timestamp;

    eyes->frame.frame_number =
        eyes->frame_count;

    /*
     * Record a bounded-rate sensory journal heartbeat instead of
     * writing a database row for every 10-fps frame. The full frame
     * remains available to the consumer; this entry documents that
     * real pixels arrived and identifies their source and format.
     */
    if (eyes->last_log_timestamp == 0 ||
        eyes->timestamp - eyes->last_log_timestamp >= 5000) {
        char details[768];
        snprintf(details, sizeof(details),
                 "source=%s; frame=%llu; resolution=%ux%u; bytes=%zu; "
                 "timestamp_ms=%llu; interpretation=not_performed_by_Eyes",
                 eyes->source_name,
                 (unsigned long long)eyes->frame_count,
                 eyes->format.width, eyes->format.height,
                 eyes->frame_size,
                 (unsigned long long)eyes->timestamp);
        r2_log_sensory("visual_frame_received",
                       "R2 Eyes received a real visual frame.",
                       details, eyes->source_name);
        eyes->last_log_timestamp = eyes->timestamp;
    }

    return 1;
}

/* ============================================================
   FRAME ACCESS
   ============================================================ */

int r2_eyes_get_frame(
    R2Eyes *eyes,
    R2VisionFrame *frame)
{
    if (!eyes ||
        !frame ||
        !eyes->open)
    {
        return -1;
    }

    *frame = eyes->frame;

    return 0;
}

ssize_t r2_eyes_copy_frame(
    R2Eyes *eyes,
    void *buffer,
    size_t buffer_size)
{
    if (!eyes ||
        !buffer ||
        !eyes->open)
    {
        return -1;
    }

    if (buffer_size < eyes->frame_size)
    {
        errno = ENOBUFS;
        return -1;
    }

    memcpy(
        buffer,
        eyes->frame_buffer,
        eyes->frame_size
    );

    return (ssize_t)eyes->frame_size;
}

/* ============================================================
   EVENT INFORMATION
   ============================================================ */

int r2_eyes_get_event(
    R2Eyes *eyes,
    R2VisionEvent *event)
{
    if (!eyes || !event)
        return -1;

    *event = eyes->event;

    return 0;
}

int r2_eyes_get_format(
    R2Eyes *eyes,
    R2VideoFormat *format)
{
    if (!eyes || !format)
        return -1;

    *format = eyes->format;

    return 0;
}

/* ============================================================
   STATE
   ============================================================ */

int r2_eyes_is_open(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->open;
}

R2VisionOrigin r2_eyes_get_origin(
    R2Eyes *eyes)
{
    if (!eyes)
        return R2_VISION_NONE;

    return eyes->origin;
}

uint64_t r2_eyes_get_frame_count(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->frame_count;
}

uint64_t r2_eyes_get_timestamp(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->timestamp;
}
