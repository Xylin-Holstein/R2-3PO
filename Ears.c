/*
 * R2-3PO Ears
 *
 * General-purpose raw audio input system for R2.
 *
 * Ears provides R2 with PCM audio and tells him where
 * that audio originated:
 *
 *     FILE     -> supplied audio/media file
 *     WORLD    -> microphone / physical environment
 *     DESKTOP  -> computer/system audio
 *     SOURCE   -> arbitrary PulseAudio/PipeWire source
 *
 * Ears does NOT interpret the audio.
 *
 * It does not:
 *     - transcribe speech
 *     - identify songs
 *     - determine emotions
 *     - recognize speakers
 *     - create memories
 *
 * Those responsibilities belong to the hearing/cognition layer.
 *
 * Ears is the input layer only.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Ears.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/simple.h>


/* ---------------------------------------------------------
 * Internal constants
 * --------------------------------------------------------- */

/*
 * Canonical PCM format used by the hearing pipeline.
 *
 * Every source is converted to:
 *
 *     48000 Hz
 *     stereo
 *     signed 16-bit little-endian PCM
 *
 * This gives the downstream hearing system one consistent
 * audio format regardless of whether the source was:
 *
 *     microphone
 *     desktop audio
 *     MP3
 *     WAV
 *     FLAC
 *     OGG
 *     movie audio
 *     game audio
 *     browser audio
 *     etc.
 */
#define R2_EARS_SAMPLE_RATE       48000
#define R2_EARS_CHANNELS          2
#define R2_EARS_BITS_PER_SAMPLE   16

#define R2_EARS_SOURCE_NAME_MAX   512

/*
 * Ten milliseconds of audio.
 *
 * This keeps reads reasonably small for real-time hearing
 * while avoiding excessive system calls.
 */
#define R2_EARS_FRAME_MS          10

#define R2_EARS_FRAME_BYTES \
    ((size_t)R2_EARS_SAMPLE_RATE * \
     (size_t)R2_EARS_CHANNELS * \
     (size_t)(R2_EARS_BITS_PER_SAMPLE / 8) * \
     R2_EARS_FRAME_MS / 1000)


/* ---------------------------------------------------------
 * Internal Ears structure
 * --------------------------------------------------------- */

struct R2Ears
{
    R2HearingEvent event;

    int open;

    /*
     * PulseAudio/PipeWire live capture.
     *
     * PipeWire normally exposes its PulseAudio compatibility
     * layer through libpulse, so this works for both systems.
     */
    pa_simple *pulse_stream;

    /*
     * FFmpeg process for file/media decoding.
     *
     * FFmpeg outputs canonical raw PCM through this pipe.
     */
    FILE *file_stream;

    pid_t file_pid;

    /*
     * Whether the current source is a file.
     */
    int using_file;

    /*
     * PCM format used internally.
     */
    R2AudioFormat format;
};


/* ---------------------------------------------------------
 * Utility functions
 * --------------------------------------------------------- */

static uint64_t r2_ears_timestamp(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return ((uint64_t)ts.tv_sec * 1000000ULL) +
           ((uint64_t)ts.tv_nsec / 1000ULL);
}


/*
 * Safely quote a string for use as one shell argument.
 *
 * This matters for:
 *
 *     /home/x/My Music/song.mp3
 *     /home/x/Movie's Audio/movie.mkv
 *
 * and also prevents a filename from becoming shell syntax.
 */
static char *r2_ears_shell_quote(const char *input)
{
    size_t length;
    size_t max_size;
    char *output;
    char *p;

    if (!input)
        return NULL;

    length = strlen(input);

    /*
     * Worst case:
     *
     * every character is a single quote and therefore becomes
     *
     *     '\''
     *
     * plus surrounding quotes.
     */
    if (length > (SIZE_MAX - 3) / 4)
        return NULL;

    max_size = (length * 4) + 3;

    output = malloc(max_size);

    if (!output)
        return NULL;

    p = output;

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


static void r2_ears_reset_event(R2Ears *ears)
{
    if (!ears)
        return;

    memset(
        &ears->event,
        0,
        sizeof(ears->event)
    );

    ears->event.origin = R2_HEARING_NONE;
    ears->event.type = R2_AUDIO_UNKNOWN;

    ears->event.format = ears->format;
    ears->event.timestamp =
        r2_ears_timestamp();
}


static void r2_ears_set_source_name(
    R2Ears *ears,
    const char *name
)
{
    if (!ears)
        return;

    if (!name)
    {
        ears->event.source_name[0] = '\0';
        return;
    }

    snprintf(
        ears->event.source_name,
        sizeof(ears->event.source_name),
        "%s",
        name
    );
}


/* ---------------------------------------------------------
 * PulseAudio / PipeWire helpers
 * --------------------------------------------------------- */

static pa_sample_spec r2_ears_sample_spec(void)
{
    pa_sample_spec spec;

    memset(
        &spec,
        0,
        sizeof(spec)
    );

    spec.format = PA_SAMPLE_S16LE;
    spec.rate = R2_EARS_SAMPLE_RATE;
    spec.channels = R2_EARS_CHANNELS;

    return spec;
}


/*
 * Get the system's default Pulse/PipeWire sink.
 *
 * Example:
 *
 *     alsa_output.pci-0000_00_1f.3.analog-stereo
 *
 * The corresponding monitor source is:
 *
 *     alsa_output.pci-0000_00_1f.3.analog-stereo.monitor
 *
 * This is what allows R2 to hear what the desktop itself
 * is playing.
 */
static char *r2_ears_get_default_sink(void)
{
    FILE *pipe;
    char buffer[1024];

    pipe = popen(
        "pactl get-default-sink 2>/dev/null",
        "r"
    );

    if (!pipe)
        return NULL;

    if (!fgets(
            buffer,
            sizeof(buffer),
            pipe
        ))
    {
        pclose(pipe);
        return NULL;
    }

    pclose(pipe);

    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (buffer[0] == '\0')
        return NULL;

    return strdup(buffer);
}


/*
 * Get the default Pulse/PipeWire recording source.
 *
 * This is more robust than simply assuming the microphone
 * has a particular device name.
 */
static char *r2_ears_get_default_source(void)
{
    FILE *pipe;
    char buffer[1024];

    pipe = popen(
        "pactl get-default-source 2>/dev/null",
        "r"
    );

    if (!pipe)
        return NULL;

    if (!fgets(
            buffer,
            sizeof(buffer),
            pipe
        ))
    {
        pclose(pipe);
        return NULL;
    }

    pclose(pipe);

    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (buffer[0] == '\0')
        return NULL;

    return strdup(buffer);
}


/*
 * Verify that a Pulse/PipeWire source exists.
 *
 * This is deliberately lightweight. The actual opening is still
 * performed by pa_simple_new().
 */
static int r2_ears_source_exists(const char *source)
{
    FILE *pipe;
    char *quoted_source;
    char command[2048];
    int found = 0;

    if (!source || source[0] == '\0')
        return 0;

    quoted_source =
        r2_ears_shell_quote(source);

    if (!quoted_source)
        return 0;

    snprintf(
        command,
        sizeof(command),
        "pactl list short sources 2>/dev/null | "
        "grep -F -- %s >/dev/null 2>&1",
        quoted_source
    );

    free(quoted_source);

    pipe = popen(command, "r");

    if (!pipe)
        return 0;

    /*
     * grep's exit status is not directly portable through the
     * stdio API, but pclose() gives us enough information here.
     */
    if (pclose(pipe) == 0)
        found = 1;

    return found;
}


/* ---------------------------------------------------------
 * File process handling
 * --------------------------------------------------------- */

static int r2_ears_start_ffmpeg(
    R2Ears *ears,
    const char *path
)
{
    char *quoted_path;
    char command[4096];
    int written;

    if (!ears || !path || path[0] == '\0')
        return -1;

    quoted_path =
        r2_ears_shell_quote(path);

    if (!quoted_path)
        return -1;

    /*
     * FFmpeg decodes any supported media source into the
     * canonical R2 PCM format.
     *
     * No speaker playback is required.
     *
     * This means R2 can directly hear:
     *
     *     WAV
     *     MP3
     *     FLAC
     *     OGG
     *     AAC
     *     M4A
     *     MP4
     *     MKV
     *     AVI
     *     etc.
     *
     * whenever FFmpeg can decode the source's audio stream.
     */
    written = snprintf(
        command,
        sizeof(command),

        "ffmpeg "
        "-nostdin "
        "-hide_banner "
        "-loglevel error "
        "-i %s "
        "-vn "
        "-f s16le "
        "-ar %u "
        "-ac %u "
        "-acodec pcm_s16le "
        "pipe:1",

        quoted_path,
        R2_EARS_SAMPLE_RATE,
        R2_EARS_CHANNELS
    );

    free(quoted_path);

    if (written < 0 ||
        (size_t)written >= sizeof(command))
    {
        return -1;
    }

    ears->file_stream =
        popen(command, "r");

    if (!ears->file_stream)
        return -1;

    ears->file_pid = -1;

    return 0;
}


/* ---------------------------------------------------------
 * Initialization
 * --------------------------------------------------------- */

int r2_ears_init(R2Ears **ears)
{
    R2Ears *new_ears;

    if (!ears)
        return -1;

    *ears = NULL;

    new_ears =
        calloc(1, sizeof(R2Ears));

    if (!new_ears)
        return -1;

    new_ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    new_ears->format.channels =
        R2_EARS_CHANNELS;

    new_ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    new_ears->open = 0;
    new_ears->pulse_stream = NULL;
    new_ears->file_stream = NULL;
    new_ears->file_pid = -1;
    new_ears->using_file = 0;

    r2_ears_reset_event(new_ears);

    *ears = new_ears;

    return 0;
}


/* ---------------------------------------------------------
 * Close current source
 * --------------------------------------------------------- */

void r2_ears_close(R2Ears *ears)
{
    if (!ears)
        return;

    if (ears->pulse_stream)
    {
        pa_simple_free(
            ears->pulse_stream
        );

        ears->pulse_stream = NULL;
    }

    if (ears->file_stream)
    {
        pclose(
            ears->file_stream
        );

        ears->file_stream = NULL;
    }

    ears->file_pid = -1;
    ears->using_file = 0;
    ears->open = 0;

    r2_ears_reset_event(ears);
}


/* ---------------------------------------------------------
 * Shutdown
 * --------------------------------------------------------- */

void r2_ears_shutdown(R2Ears *ears)
{
    if (!ears)
        return;

    r2_ears_close(ears);

    free(ears);
}


/* ---------------------------------------------------------
 * Open generic Pulse/PipeWire source
 * --------------------------------------------------------- */

int r2_ears_open_source(
    R2Ears *ears,
    const char *source_name
)
{
    pa_sample_spec spec;
    pa_buffer_attr buffer_attr;
    int error = 0;
    char *resolved_source = NULL;
    const char *actual_source;

    if (!ears)
        return -1;

    /*
     * NULL means:
     *
     *     "use the system's default recording source."
     *
     * Usually this is the microphone.
     */
    if (!source_name ||
        source_name[0] == '\0')
    {
        resolved_source =
            r2_ears_get_default_source();

        actual_source =
            resolved_source;
    }
    else
    {
        actual_source = source_name;
    }

    if (!actual_source ||
        actual_source[0] == '\0')
    {
        free(resolved_source);

        fprintf(
            stderr,
            "R2 Ears: no default audio source available.\n"
        );

        return -1;
    }

    r2_ears_close(ears);

    spec =
        r2_ears_sample_spec();

    memset(
        &buffer_attr,
        0,
        sizeof(buffer_attr)
    );

    /*
     * Pulse/PipeWire can choose the actual internal buffer
     * layout, while fragsize gives R2 roughly 10 ms reads.
     */
    buffer_attr.maxlength =
        (uint32_t)-1;

    buffer_attr.fragsize =
        (uint32_t)R2_EARS_FRAME_BYTES;

    ears->pulse_stream =
        pa_simple_new(
            NULL,
            "R2-3PO",
            PA_STREAM_RECORD,
            actual_source,
            "R2 Hearing",
            &spec,
            NULL,
            &buffer_attr,
            &error
        );

    if (!ears->pulse_stream)
    {
        fprintf(
            stderr,
            "R2 Ears: could not open audio source '%s': %s\n",
            actual_source,
            pa_strerror(error)
        );

        free(resolved_source);

        return -1;
    }

    ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    ears->format.channels =
        R2_EARS_CHANNELS;

    ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    ears->event.origin =
        R2_HEARING_WORLD;

    ears->event.type =
        R2_AUDIO_UNKNOWN;

    ears->event.format =
        ears->format;

    ears->event.timestamp =
        r2_ears_timestamp();

    r2_ears_set_source_name(
        ears,
        actual_source
    );

    ears->using_file = 0;
    ears->open = 1;

    free(resolved_source);

    return 0;
}


/* ---------------------------------------------------------
 * Open default microphone / recording source
 * --------------------------------------------------------- */

int r2_ears_open_microphone(R2Ears *ears)
{
    if (!ears)
        return -1;

    return r2_ears_open_source(
        ears,
        NULL
    );
}


/* ---------------------------------------------------------
 * Open desktop/system audio
 * --------------------------------------------------------- */

int r2_ears_open_desktop(R2Ears *ears)
{
    char *sink;
    char monitor[1024];
    int result;

    if (!ears)
        return -1;

    sink =
        r2_ears_get_default_sink();

    if (!sink)
    {
        fprintf(
            stderr,
            "R2 Ears: could not determine the default "
            "desktop audio sink.\n"
        );

        return -1;
    }

    /*
     * The Pulse/PipeWire monitor source represents the audio
     * being sent through the sink.
     *
     * Therefore:
     *
     *     MP3 playing on desktop
     *     YouTube playing in browser
     *     game audio
     *     movie audio
     *     system sounds
     *
     * all appear here.
     */
    snprintf(
        monitor,
        sizeof(monitor),
        "%s.monitor",
        sink
    );

    free(sink);

    /*
     * Give a useful diagnostic before attempting to open it.
     */
    if (!r2_ears_source_exists(monitor))
    {
        /*
         * Do NOT immediately fail.
         *
         * PipeWire/Pulse implementations can expose sources in
         * ways that make pactl's listing differ from what
         * libpulse can actually open.
         *
         * Let pa_simple_new() make the final determination.
         */
    }

    result =
        r2_ears_open_source(
            ears,
            monitor
        );

    if (result != 0)
        return result;

    ears->event.origin =
        R2_HEARING_WORLD;

    r2_ears_set_source_name(
        ears,
        "desktop-audio"
    );

    return 0;
}


/* ---------------------------------------------------------
 * Open arbitrary audio file
 * --------------------------------------------------------- */

int r2_ears_open_file(
    R2Ears *ears,
    const char *path
)
{
    if (!ears ||
        !path ||
        path[0] == '\0')
    {
        return -1;
    }

    r2_ears_close(ears);

    if (r2_ears_start_ffmpeg(
            ears,
            path
        ) != 0)
    {
        fprintf(
            stderr,
            "R2 Ears: could not open audio file: %s\n",
            path
        );

        return -1;
    }

    ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    ears->format.channels =
        R2_EARS_CHANNELS;

    ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    /*
     * R2 knows that this audio originated as a file.
     *
     * It does NOT know what the audio means.
     */
    ears->event.origin =
        R2_HEARING_FILE;

    ears->event.type =
        R2_AUDIO_UNKNOWN;

    ears->event.format =
        ears->format;

    ears->event.timestamp =
        r2_ears_timestamp();

    r2_ears_set_source_name(
        ears,
        path
    );

    ears->using_file = 1;
    ears->open = 1;

    return 0;
}


/* ---------------------------------------------------------
 * Read PCM audio
 * --------------------------------------------------------- */

ssize_t r2_ears_read(
    R2Ears *ears,
    void *buffer,
    size_t buffer_size
)
{
    if (!ears ||
        !buffer ||
        buffer_size == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (!ears->open)
    {
        errno = EBADF;
        return -1;
    }


    /* -----------------------------------------------------
     * File source
     * ----------------------------------------------------- */

    if (ears->using_file)
    {
        size_t bytes_read;

        if (!ears->file_stream)
        {
            errno = EBADF;
            return -1;
        }

        bytes_read =
            fread(
                buffer,
                1,
                buffer_size,
                ears->file_stream
            );

        if (bytes_read > 0)
        {
            ears->event.timestamp =
                r2_ears_timestamp();

            return (ssize_t)bytes_read;
        }

        if (feof(ears->file_stream))
        {
            r2_ears_close(ears);

            return 0;
        }

        if (ferror(ears->file_stream))
        {
            r2_ears_close(ears);

            return -1;
        }

        return 0;
    }


    /* -----------------------------------------------------
     * PulseAudio / PipeWire source
     * ----------------------------------------------------- */

    if (ears->pulse_stream)
    {
        int error = 0;

        if (pa_simple_read(
                ears->pulse_stream,
                buffer,
                buffer_size,
                &error
            ) < 0)
        {
            fprintf(
                stderr,
                "R2 Ears: audio read failed: %s\n",
                pa_strerror(error)
            );

            return -1;
        }

        ears->event.timestamp =
            r2_ears_timestamp();

        return (ssize_t)buffer_size;
    }


    errno = EBADF;

    return -1;
}


/* ---------------------------------------------------------
 * Event information
 * --------------------------------------------------------- */

int r2_ears_get_event(
    R2Ears *ears,
    R2HearingEvent *event
)
{
    if (!ears || !event)
        return -1;

    memcpy(
        event,
        &ears->event,
        sizeof(R2HearingEvent)
    );

    return 0;
}


/* ---------------------------------------------------------
 * Audio format
 * --------------------------------------------------------- */

int r2_ears_get_format(
    R2Ears *ears,
    R2AudioFormat *format
)
{
    if (!ears || !format)
        return -1;

    memcpy(
        format,
        &ears->format,
        sizeof(R2AudioFormat)
    );

    return 0;
}


/* ---------------------------------------------------------
 * State
 * --------------------------------------------------------- */

int r2_ears_is_open(R2Ears *ears)
{
    if (!ears)
        return 0;

    return ears->open;
}


R2HearingOrigin r2_ears_get_origin(R2Ears *ears)
{
    if (!ears)
        return R2_HEARING_NONE;

    return ears->event.origin;
}
