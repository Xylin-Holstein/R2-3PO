# R2 state-aware sound effects

R2's conversation engine can emit one private control marker when a brief droid sound genuinely adds meaning:

- `[R2_SOUND:beep:curious]`
- `[R2_SOUND:whistle:happy]`

The runtime removes the marker before the reply is saved or shown. The marker's state tag is matched against filename tokens; it does not cause R2 to claim that a physical event happened. Routine replies should not trigger sounds.

## Runtime files and playback

The default FX directory is:

`/home/x/R2_Home/R2_sounds/FX`

Set `R2_FX_DIR` in the environment before starting `R2_Launch_Code.sh` to override it for a test or alternate installation; the launcher forwards that value through the switch to the `r2` Linux account. Before launching R2, the launcher checks that `r2` can read the directory and finds at least one MP3, printing a warning rather than preventing startup when assets are missing or inaccessible. R2 scans only existing `.mp3` files; it never generates or renames sound files. Names should contain a sound kind token (`beep`, `boop`, `bleep`, or `whistle`) and, where available, a state token such as `curious`, `happy`, `confused`, `alert`, `sleepy`, `hungry`, `excited`, `thinking`, `greeting`, or `default`. A state-specific file wins over a generic/default file. Matching is by filename tokens, not arbitrary substrings; ties are resolved deterministically.

Playback is asynchronous and will not overlap another R2 sound effect. The runtime tries existing local players in this order: `mpg123`, `ffplay`, then `mpv`. It does not install packages automatically. If no matching file or player is available, the spoken reply continues normally and no fabricated sound is substituted. A short cooldown prevents repeated sound spam.

For automated tests, `R2_SOUNDS_DISABLE_PLAYBACK=1` disables the player while keeping file selection and marker cleanup testable.

## Validation boundary

CI exercises filename matching and marker removal using temporary fixture filenames; it does not decode audio or verify a real speaker. The MP3 assets need to exist in the runtime FX directory, and playback still needs a quick check on R2's actual machine. The current GitHub tree I could inspect did not expose the MP3 assets, so the real filenames and state coverage have not yet been checked against the user's sound collection.
