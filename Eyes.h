#ifndef R2_EYES_H
#define R2_EYES_H

/*
 * R2-3PO Eyes
 *
 * Raw visual input system for R2.
 *
 * Eyes provides R2 with video frames and tells him where
 * those frames originated:
 *
 *     FILE -> video/image supplied from a file
 *     WORLD -> camera/environment
 *     VLC -> visible VLC media window
 *
 * Eyes does NOT interpret the image.
 *
 * It does not:
 *     - recognize faces
 *     - identify objects
 *     - read text
 *     - describe scenes
 *     - determine emotions
 *     - create memories
 *
 * Those responsibilities belong to R2's vision/cognition layer.
 *
 * Eyes and Ears are designed to work together. Their events
 * carry timestamps so R2 can associate what he sees with
 * what he hears as one experience.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Vision origin */
typedef enum
{
    R2_VISION_NONE = 0,
    R2_VISION_FILE,
    R2_VISION_WORLD,
    R2_VISION_VLC
} R2VisionOrigin;

/* Visual type */
typedef enum
{
    R2_VISUAL_UNKNOWN = 0,
    R2_VISUAL_IMAGE,
    R2_VISUAL_VIDEO,
    R2_VISUAL_SCENE,
    R2_VISUAL_MIXED
} R2VisualType;

/* Pixel format */
typedef enum
{
    R2_PIXEL_UNKNOWN = 0,
    R2_PIXEL_RGB24,
    R2_PIXEL_BGR24,
    R2_PIXEL_RGBA32,
    R2_PIXEL_BGRA32
} R2PixelFormat;

/* Video/image format */
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint16_t bits_per_pixel;
    R2PixelFormat pixel_format;
} R2VideoFormat;

/* Vision frame */
typedef struct
{
    const unsigned char *data;
    size_t size;
    R2VideoFormat format;
    uint64_t timestamp;
    uint64_t frame_number;
} R2VisionFrame;

/* Vision event */
typedef struct
{
    R2VisionOrigin origin;
    R2VisualType type;
    char source_name[512];
    R2VideoFormat format;
    uint64_t timestamp;
    uint64_t frame_count;
} R2VisionEvent;

/* Opaque Eyes object */
typedef struct R2Eyes R2Eyes;

/* Initialization / shutdown */
int r2_eyes_init(R2Eyes **eyes);
void r2_eyes_shutdown(R2Eyes *eyes);

/* Input sources */
int r2_eyes_open_file(R2Eyes *eyes, const char *path);
int r2_eyes_open_camera(R2Eyes *eyes);
int r2_eyes_open_device(R2Eyes *eyes,
                        const char *device);
int r2_eyes_open_source(R2Eyes *eyes,
                        const char *source_name);

/* VLC window input */
int r2_eyes_open_vlc(R2Eyes *eyes);

void r2_eyes_close(R2Eyes *eyes);

/* Frame acquisition */
int r2_eyes_capture(R2Eyes *eyes);
int r2_eyes_get_frame(R2Eyes *eyes,
                      R2VisionFrame *frame);

ssize_t r2_eyes_copy_frame(
    R2Eyes *eyes,
    void *buffer,
    size_t buffer_size
);

/* Event information */
int r2_eyes_get_event(
    R2Eyes *eyes,
    R2VisionEvent *event
);

int r2_eyes_get_format(
    R2Eyes *eyes,
    R2VideoFormat *format
);

/* State */
int r2_eyes_is_open(R2Eyes *eyes);

R2VisionOrigin r2_eyes_get_origin(
    R2Eyes *eyes
);

uint64_t r2_eyes_get_frame_count(
    R2Eyes *eyes
);

/* Synchronization */
uint64_t r2_eyes_get_timestamp(
    R2Eyes *eyes
);

#endif
