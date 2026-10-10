#ifndef R2_EARS_H
#define R2_EARS_H

/*
 * R2-3PO Ears
 *
 * Ears provides raw PCM input and records its source. For explicit,
 * user-triggered listening, it can capture a short clip and pass a WAV to
 * R2's shared local Gemma 4 model for transcription and cautious sound
 * description. It does not identify a person by voice and does not silently
 * record in the background.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>


/* ---------------------------------------------------------
 * Hearing origin
 * --------------------------------------------------------- */

typedef enum
{
    R2_HEARING_NONE = 0,

    /* Audio intentionally supplied from a file. */
    R2_HEARING_FILE,

    /* Audio captured from the physical environment. */
    R2_HEARING_WORLD

} R2HearingOrigin;


/* ---------------------------------------------------------
 * What kind of audio R2 believes he is hearing
 *
 * Ears itself does not have to determine this.
 * This exists so the later hearing/cognition layer
 * can classify an incoming experience.
 * --------------------------------------------------------- */

typedef enum
{
    R2_AUDIO_UNKNOWN = 0,

    R2_AUDIO_SPEECH,
    R2_AUDIO_MUSIC,
    R2_AUDIO_SOUND,
    R2_AUDIO_MIXED

} R2AudioType;


/* ---------------------------------------------------------
 * PCM audio format
 *
 * R2's internal hearing pipeline will use PCM audio.
 * Keeping a defined format lets files, desktop audio,
 * and physical microphones feed the same system.
 * --------------------------------------------------------- */

typedef struct
{
    uint32_t sample_rate;       /* Samples per second. */
    uint16_t channels;          /* 1 = mono, 2 = stereo, etc. */
    uint16_t bits_per_sample;   /* Usually 16. */

} R2AudioFormat;


/* ---------------------------------------------------------
 * Hearing event
 *
 * This is the information R2 receives alongside audio.
 *
 * Example:
 *
 *   origin = R2_HEARING_FILE
 *   source = "song.mp3"
 *
 * means:
 *
 *   "R2, what you are hearing originated from a file."
 *
 * It does NOT mean:
 *
 *   "R2, this is music."
 *
 * The hearing/cognition layer determines that separately.
 * --------------------------------------------------------- */

typedef struct
{
    R2HearingOrigin origin;

    /*
     * Classification of the actual audio.
     *
     * This may initially be R2_AUDIO_UNKNOWN.
     * The hearing/cognition system can later classify it.
     */
    R2AudioType type;

    /*
     * Human-readable source identifier.
     *
     * Examples:
     *   "song.mp3"
     *   "microphone"
     *   "desktop-audio"
     */
    char source_name[512];

    R2AudioFormat format;

    /*
     * Timestamp associated with the beginning
     * of this hearing event.
     */
    uint64_t timestamp;

} R2HearingEvent;


/* ---------------------------------------------------------
 * Opaque Ears object
 *
 * The implementation details stay inside Ears.c.
 * --------------------------------------------------------- */

typedef struct R2Ears R2Ears;


/* ---------------------------------------------------------
 * Initialization / shutdown
 * --------------------------------------------------------- */

/*
 * Creates and initializes R2's hearing system.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int r2_ears_init(R2Ears **ears);


/*
 * Shuts down the hearing system and releases resources.
 */
void r2_ears_shutdown(R2Ears *ears);


/* ---------------------------------------------------------
 * Input sources
 * --------------------------------------------------------- */

/*
 * Open an audio file as R2's hearing source.
 *
 * The decoded audio will ultimately enter R2 as PCM,
 * just like microphone audio.
 */
int r2_ears_open_file(
    R2Ears *ears,
    const char *path
);


/*
 * Open the physical/default microphone.
 *
 * This represents the outside world.
 */
int r2_ears_open_microphone(
    R2Ears *ears
);


/*
 * Open a specific system audio source.
 *
 * This will allow R2 to hear things such as desktop audio
 * through a PipeWire/PulseAudio monitor source.
 */
int r2_ears_open_source(
    R2Ears *ears,
    const char *source_name
);


/*
 * Open the desktop's current audio output.
 *
 * This allows R2 to hear audio playing on the computer
 * without requiring that audio to physically come through
 * the speakers.
 */
int r2_ears_open_desktop(
    R2Ears *ears
);


/*
 * Stop the current hearing source.
 */
void r2_ears_close(
    R2Ears *ears
);


/* ---------------------------------------------------------
 * Hearing
 * --------------------------------------------------------- */

/*
 * Read raw PCM audio from R2's current hearing source.
 *
 * Returns:
 *   > 0  = number of bytes received
 *    0   = end of input
 *   -1   = error
 */
ssize_t r2_ears_read(
    R2Ears *ears,
    void *buffer,
    size_t buffer_size
);


/*
 * Retrieve information about the current hearing event.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int r2_ears_get_event(
    R2Ears *ears,
    R2HearingEvent *event
);


/*
 * Retrieve the PCM format currently being delivered.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int r2_ears_get_format(
    R2Ears *ears,
    R2AudioFormat *format
);


/* ---------------------------------------------------------
 * State
 * --------------------------------------------------------- */

/*
 * Returns non-zero if R2 currently has an active
 * hearing source.
 */
int r2_ears_is_open(
    R2Ears *ears
);

/* Explicitly capture and interpret 1-30 seconds from the current source.
   If no source is open, this temporarily opens the default microphone.
   The transcript/description is recorded in the existing memory and Life Log.
   Returns caller-owned text, or NULL on capture/model failure. */
char *r2_ears_listen_and_interpret(R2Ears *ears, unsigned seconds);


/*
 * Returns the origin of the current audio source.
 */
R2HearingOrigin r2_ears_get_origin(
    R2Ears *ears
);


#endif /* R2_EARS_H */
