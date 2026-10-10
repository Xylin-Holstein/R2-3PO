#define _GNU_SOURCE
#include "R2Sounds.h"
#include "Log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int r2_log_is_initialized(void) { return 0; }
int64_t r2_log_event(R2LogCategory category, const char *event_type,
                     const char *summary, const char *details, const char *source)
{
    (void)category; (void)event_type; (void)summary; (void)details; (void)source;
    return -1;
}

static void touch(const char *dir, const char *name)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *fp = fopen(path, "wb");
    assert(fp);
    fputs("test fixture; selection only, not a playable MP3", fp);
    fclose(fp);
}

int main(void)
{
    char root[] = "/tmp/r2-fx-smoke-XXXXXX";
    assert(mkdtemp(root));
    touch(root, "R2_Beep_Curious.mp3");
    touch(root, "R2_Beep_Sleepy.mp3");
    touch(root, "R2_Beep_Default.mp3");
    touch(root, "R2_Whistle_Happy.mp3");
    touch(root, "R2_Whistle_Default.mp3");
    touch(root, "not_a_sound.wav");
    touch(root, "R2_Beep_Curious.txt");
    char fake_dir[4096];
    snprintf(fake_dir, sizeof(fake_dir), "%s/R2_Beep_Silly.mp3", root);
    assert(mkdir(fake_dir, 0700) == 0);

    char path[4096];
    assert(r2_sounds_select_file(root, "beep", "curious", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Curious.mp3"));
    assert(r2_sounds_select_file(root, "beep", "silly", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Default.mp3"));
    assert(r2_sounds_select_file(root, "beep", "sleepy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy.mp3"));
    assert(r2_sounds_select_file(root, "beep", "tired", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy.mp3"));
    assert(r2_sounds_select_file(root, "whistle", "happy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Happy.mp3"));
    assert(r2_sounds_select_file(root, "whistle", "confused", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Default.mp3"));
    assert(r2_sounds_select_file(root, "laugh", "happy", path, sizeof(path)) != 0);
    assert(r2_sounds_select_file(root, "beep", "curious", path, 2) != 0);

    assert(setenv("R2_FX_DIR", root, 1) == 0);
    assert(setenv("R2_SOUNDS_DISABLE_PLAYBACK", "1", 1) == 0);
    const char *reply = "Hello! [R2_SOUND:beep:curious] I found the right effect. [R2_SOUND:whistle:happy]";
    char *clean = r2_sounds_process_reply(reply);
    assert(clean);
    assert(!strstr(clean, "[R2_SOUND:"));
    assert(strstr(clean, "Hello!") && strstr(clean, "I found the right effect."));
    free(clean);

    clean = r2_sounds_process_reply("Plain reply with no marker.");
    assert(clean && !strcmp(clean, "Plain reply with no marker."));
    free(clean);

    char oversized_marker[320];
    char long_state[220];
    memset(long_state, 'x', sizeof(long_state) - 1);
    long_state[sizeof(long_state) - 1] = '\0';
    snprintf(oversized_marker, sizeof(oversized_marker),
             "Before [R2_SOUND:beep:%s] after", long_state);
    clean = r2_sounds_process_reply(oversized_marker);
    assert(clean && !strstr(clean, "[R2_SOUND:") && strstr(clean, "Before ") &&
           strstr(clean, " after"));
    free(clean);

    /* Use a fake local decoder to verify the asynchronous player can be
       stopped/reaped without requiring actual MP3 playback or audio hardware. */
    assert(unsetenv("R2_SOUNDS_DISABLE_PLAYBACK") == 0);
    char player_path[4096];
    snprintf(player_path, sizeof(player_path), "%s/mpg123", root);
    FILE *player = fopen(player_path, "w");
    assert(player);
    fputs("#!/bin/sh\nexec /bin/sleep 30\n", player);
    fclose(player);
    assert(chmod(player_path, 0700) == 0);
    assert(setenv("PATH", root, 1) == 0);
    /* An unknown model-invented state must be stripped without triggering a
       generic sound; otherwise the following real play would be rate-limited. */
    clean = r2_sounds_process_reply("Before [R2_SOUND:beep:banana] after");
    assert(clean && !strstr(clean, "[R2_SOUND:") && strstr(clean, "Before ") &&
           strstr(clean, " after"));
    free(clean);
    assert(r2_sounds_play("beep", "curious") == 0);
    r2_sounds_shutdown();
    unlink(player_path);

    const char *const names[] = {
        "R2_Beep_Curious.mp3", "R2_Beep_Sleepy.mp3", "R2_Beep_Default.mp3",
        "R2_Whistle_Happy.mp3", "R2_Whistle_Default.mp3",
        "not_a_sound.wav", "R2_Beep_Curious.txt"
    };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        char file[4096];
        snprintf(file, sizeof(file), "%s/%s", root, names[i]);
        unlink(file);
    }
    rmdir(fake_dir);
    rmdir(root);
    puts("R2 state-aware sound smoke passed");
    return 0;
}
