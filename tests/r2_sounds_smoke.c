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
    touch(root, "R2_Whistle_Acknowledgement.mp3");
    touch(root, "R2_Whistle_Default.mp3");
    touch(root, "not_a_sound.wav");
    touch(root, "R2_Beep_Curious.txt");
    char fake_dir[4096];
    snprintf(fake_dir, sizeof(fake_dir), "%s/R2_Beep_Silly.mp3", root);
    assert(mkdir(fake_dir, 0700) == 0);

    char path[4096];
    assert(r2_sounds_select_file(root, "beep", "curious", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Curious.mp3"));
    /* State vocabulary belongs to R2, not to a hard-coded allowlist. An
       unfamiliar label may use a generic asset when no matching clip exists. */
    assert(r2_sounds_select_file(root, "beep", "banana", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Default.mp3"));
    assert(r2_sounds_select_file(root, "beep", "silly", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Default.mp3"));
    assert(r2_sounds_select_file(root, "beep", "sleepy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy.mp3"));
    assert(r2_sounds_select_file(root, "beep", "tired", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy.mp3"));
    assert(r2_sounds_select_file(root, "whistle", "happy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Happy.mp3"));
    /* Alias matching must work in both directions, not just for one canonical
       spelling of each state. */
    assert(r2_sounds_select_file(root, "whistle", "cheerful", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Happy.mp3"));
    assert(r2_sounds_select_file(root, "whistle", "greeting", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Acknowledgement.mp3"));
    assert(r2_sounds_select_file(root, "whistle", "confused", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Whistle_Default.mp3"));

    /* A mismatched state-labeled clip must not be used when no generic clip
       exists; otherwise a curious R2 could accidentally sound sleepy. */
    char mismatch_dir[] = "/tmp/r2-fx-mismatch-XXXXXX";
    assert(mkdtemp(mismatch_dir));
    touch(mismatch_dir, "R2_Beep_Sleepy.mp3");
    assert(r2_sounds_select_file(mismatch_dir, "beep", "curious", path, sizeof(path)) != 0);
    assert(r2_sounds_select_file(mismatch_dir, "beep", "sleepy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy.mp3"));
    char conflict_dir[] = "/tmp/r2-fx-conflict-XXXXXX";
    assert(mkdtemp(conflict_dir));
    touch(conflict_dir, "R2_Beep_Sleepy_Default.mp3");
    /* A generic suffix must not disguise a clip explicitly tagged for a
       conflicting state. */
    assert(r2_sounds_select_file(conflict_dir, "beep", "curious", path, sizeof(path)) != 0);
    assert(r2_sounds_select_file(conflict_dir, "beep", "sleepy", path, sizeof(path)) == 0);
    assert(strstr(path, "R2_Beep_Sleepy_Default.mp3"));
    char conflict_file[4096];
    snprintf(conflict_file, sizeof(conflict_file), "%s/R2_Beep_Sleepy_Default.mp3", conflict_dir);
    unlink(conflict_file);
    rmdir(conflict_dir);

    char mismatch_file[4096];
    snprintf(mismatch_file, sizeof(mismatch_file), "%s/R2_Beep_Sleepy.mp3", mismatch_dir);
    unlink(mismatch_file);
    rmdir(mismatch_dir);

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

    clean = r2_sounds_process_reply("Open state [R2_SOUND:beep:banana] label.");
    assert(clean && !strstr(clean, "[R2_SOUND:") && strstr(clean, "Open state ") &&
           strstr(clean, " label."));
    free(clean);

    /* Marker syntax quoted as code is documentation, not a hidden action. */
    clean = r2_sounds_process_reply(
        "The literal syntax is `[R2_SOUND:beep:curious]`.");
    assert(clean && strstr(clean, "`[R2_SOUND:beep:curious]`"));
    free(clean);

    clean = r2_sounds_process_reply(
        "Example:\n```text\n[R2_SOUND:whistle:happy]\n```\nNo sound was requested.");
    assert(clean && strstr(clean, "[R2_SOUND:whistle:happy]") &&
           strstr(clean, "No sound was requested."));
    free(clean);

    /* Active markers still disappear even when an earlier example is quoted. */
    clean = r2_sounds_process_reply(
        "Example `[R2_SOUND:beep:curious]`; hello [R2_SOUND:whistle:happy]");
    assert(clean && strstr(clean, "`[R2_SOUND:beep:curious]`") &&
           !strstr(clean, "[R2_SOUND:whistle:happy]") &&
           strstr(clean, "hello "));
    free(clean);

    /* Model output casing can vary; control syntax must never leak as prose. */
    clean = r2_sounds_process_reply("Before [r2_sound:beep:curious] after");
    assert(clean && !strcasestr(clean, "[r2_sound:") &&
           strstr(clean, "Before ") && strstr(clean, " after"));
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
    /* Ignore SIGTERM deliberately; shutdown must escalate to SIGKILL
       after a bounded grace period instead of hanging forever. */
    fputs("#!/bin/sh\ntrap '' TERM\nexec /bin/sleep 30\n", player);
    fclose(player);
    assert(chmod(player_path, 0700) == 0);
    assert(setenv("PATH", root, 1) == 0);
    clean = r2_sounds_strip_markers("Before [R2_SOUND:beep:curious] after");
    assert(clean && !strstr(clean, "[R2_SOUND:") &&
           strstr(clean, "Before ") && strstr(clean, " after"));
    free(clean);

    /* A marker alone is not a user-facing answer and must not play. */
    clean = r2_sounds_process_reply("[R2_SOUND:beep:curious]");
    assert(clean == NULL);
    /* Strip-only processing must not start a player or consume the rate limit. */
    assert(r2_sounds_play("beep", "curious") == 0);
    r2_sounds_shutdown();
    assert(r2_sounds_play("beep", "curious") != 0);
    /* The public core lifecycle permits a later init in the same process. */
    r2_sounds_init();
    assert(r2_sounds_play("beep", "curious") == 0);
    r2_sounds_shutdown();
    unlink(player_path);

    const char *const names[] = {
        "R2_Beep_Curious.mp3", "R2_Beep_Sleepy.mp3", "R2_Beep_Default.mp3",
        "R2_Whistle_Happy.mp3", "R2_Whistle_Acknowledgement.mp3", "R2_Whistle_Default.mp3",
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
