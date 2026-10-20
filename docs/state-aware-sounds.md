# R2 state-aware sound effects

R2's conversation engine can emit one private control marker when a brief droid sound genuinely adds meaning:

- `[R2_SOUND:beep:curious]`
- `[R2_SOUND:whistle:happy]`

The runtime removes the marker before the reply is saved or shown. Tool-routing replies have their markers stripped without playback; only the final user-facing reply can trigger an effect. Marker parsing is case-insensitive, and if safe cleanup fails, the turn is discarded rather than exposing control text. The marker's state tag is matched against filename tokens; it does not cause R2 to claim that a physical event happened. R2 may choose any single alphanumeric state label; the sound layer does not impose an emotional vocabulary. Exact filename matches win, known synonyms help asset lookup, and a generic/default or unlabelled asset may be used when no explicit conflicting state label exists. Routine replies should not trigger sounds. State labels must be grounded in the current turn or current Reality evidence; a sound tag is a selection hint, not proof of a hidden emotional state.

## Runtime files and playback

The default FX directory is:

`/home/x/R2_Home/R2_sounds/FX`

Set `R2_FX_DIR` in the environment before starting `R2_Launch_Code.sh` to override it for a test or alternate installation; the launcher forwards that value through the switch to the `r2` Linux account. Before launching R2, the launcher checks that `r2` can read the directory and finds at least one MP3, printing a warning rather than preventing startup when assets are missing or inaccessible. R2 scans only existing `.mp3` files; it never generates or renames sound files. Names should contain a sound kind token (`beep`, `boop`, `bleep`, or `whistle`) and, where available, the state token R2 chose. The runtime recognizes exact custom state labels without adding them to a fixed list; known synonym groups help match legacy filenames. An exact or documented-alias state match wins first, followed by a generic/default file, then an unlabelled kind-specific fallback. A clip explicitly tagged for a different state is never substituted. Matching is by filename tokens, not arbitrary substrings; ties are resolved deterministically.

Playback is asynchronous and will not overlap another R2 sound effect. `tools/audit_r2_sound_assets.py --root /home/x/R2_Home/R2_sounds/FX` performs a read-only inventory: it reports readable MP3s, recognized beep/whistle filename tokens, state-specific or generic coverage, and installed player commands. The launcher runs this inventory as the `r2` account when the script is deployed under `R2/tools/`; otherwise it uses a simpler access check. The audit never modifies files or decodes audio.

The runtime tries existing local players in this order: `mpg123`, `ffplay`, then `mpv`. It does not install packages automatically. If no matching file or player is available, the spoken reply continues normally and no fabricated sound is substituted. A short cooldown prevents repeated sound spam.

For automated tests, `R2_SOUNDS_DISABLE_PLAYBACK=1` disables the player while keeping file selection and marker cleanup testable.

## Validation boundary

CI exercises filename matching and marker removal using temporary fixture filenames; it does not decode audio or verify a real speaker. The FX collection is a runtime installation asset on the host machine, not a required repository directory. On R2's machine, the read-only inventory script checks the actual local filenames and state coverage; real speaker playback still needs a quick on-device check.
