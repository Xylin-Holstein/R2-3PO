# R2-3PO

R2-3PO is a C program with separate core, shell, diary, life-log, visual, eye, and hearing modules.

## Build on Ubuntu/Debian

Install the compiler and development libraries:

```sh
sudo apt-get update
sudo apt-get install -y build-essential libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libpulse-dev
```

Compile the complete C application from the repository root:

```sh
make clean all
```

The resulting executable is `./r2`. To remove it, run `make clean`.

## Ollama model: conversation and vision

R2's default conversation model and visual-perception model are both `gemma4:e2b`.
Install the model locally before launching R2:

```sh
ollama pull gemma4:e2b
ollama run gemma4:e2b
```

At the Ollama prompt, test a short message and then enter `/bye`. The launch script
also reports the expected model name. R2 uses the same model for text and image input. Ears currently captures PCM audio only; it does not yet transcribe or submit audio to Ollama;
visual inference shares the Ollama request lock with conversation and yields when a
foreground conversation is waiting. Ordinary conversation does not trigger a new frame
analysis unless the user asks about visual context; ongoing Eyes/VLC observation can
continue through its existing watcher. If a vision request cannot run, its incomplete
result is discarded rather than being treated as a valid observation.

R2 builds a completed conversational response from the current user message and
relevant memories/perceptions. A separate intent-summary inference is not used as an
intermediate replacement for the answer. Build and smoke tests do not replace a live
test with the locally installed Ollama model and actual Eyes/TV setup.

You can also compile directly with the same source list and libraries:

```sh
gcc -std=c11 -Wall -Wextra -O2 \
  r2.c shell.c r2_diary.c Log.c Reality.c Addiction.c Reward.c AlternateSelf.c Imagination.c Visual.c Ears.c Eyes.c \
  -o r2 \
  -lcurl -lsqlite3 -lpthread -ljson-c -lpulse-simple -lpulse -lm
```

## Compilation source inventory

The build requires these twelve C translation units:

- `r2.c` — core, conversation, shared Ollama interface, and memory retrieval
- `shell.c` — command shell
- `r2_diary.c` — private diary and historical reconnection
- `Log.c` — factual Life Log chronology and event links
- `Reality.c` — persistent self-continuity, world state, room objects, needs, and inventory containers
- `Addiction.c` — persistent enjoyment history and behavior-derived repeated-interest/addiction labels
- `Reward.c` — durable learning/reinforcement ledger
- `AlternateSelf.c` — persistent, explicitly hypothetical Choice Lab branches
- `Imagination.c` — context-grounded hypothetical experiences, persisted through the Choice Lab rather than a parallel database
- `Visual.c` — vision-model integration and visual experience library
- `Ears.c` — raw audio input and source metadata
- `Eyes.c` — visual input

Their matching headers are included in the repository: `r2.h`, `shell.h`, `r2_diary.h`, `Log.h`, `Reality.h`, `Addiction.h`, `Reward.h`, `AlternateSelf.h`, `Imagination.h`, `Visual.h`, `Ears.h`, and `Eyes.h`.

GitHub Actions checks shell-script syntax and compiles this source set on pushes to the audit and combined integration branches and the configured main branches. It also runs isolated smoke tests for file-authoritative money (seed, deletion, restart, deposit/withdrawal, purchase rollback, and cents) and the GBA launcher/lifecycle. These tests do not exercise Ollama, live audio/video devices, or the user's actual graphical desktop.


## Choice Lab, age, senses, and money

The Choice Lab is open-ended, not an enumerated menu: R2 can consider any proposed choice using current needs, memories, self-facts, and learned preferences. The existing Alternate-Self Lab persists hypothetical branches separately from factual events. R2 can use the shell commands `alternate list`, `alternate show <id>`, `alternate create name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID`, `alternate compare <id> <id>`, `alternate retain <id>`, and `alternate discard <id>`. The model can list/show/compare branches through `[ALTERNATE_LIST]`, `[ALTERNATE_SHOW] id`, and `[ALTERNATE_COMPARE] id id`, and save open-ended alternatives through `[ALTERNATE_CREATE] name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID [END ALTERNATE_CREATE]`. There is no hardcoded menu of choices. Stored branches remain hypothetical and are separate from factual memories; explicitly stated hypotheticals extracted from responses are imported into the lab without duplicating their Life Log event.

Sensory counterfactuals can ask what a hypothetical view, sound, taste, smell, or touch might reveal without activating Eyes/Ears or changing the world. R2 receives recent Life Log evidence alongside current needs, memories, self-facts, learned food/ingredient preferences, and taste metrics. He must label imagined details as predictions, distinguish observations from guesses, and never update preferences as though an imagined sensation actually occurred. Only real experiences or explicit feedback update learned preferences. Food enjoyment is mapped from R2's accumulated average -2..+2 food ratings into the shared 0..100 habit score; unrated food starts neutral, and satiety/fullness remains a separate need value.

R2's age is measured from the filesystem birth/creation time of `R2/r2_original_conversation.txt` (the original conversation archive), saved in Reality metadata so later edits do not reset it. If the file is absent or the filesystem does not expose a creation timestamp, age is left unestablished rather than guessed from modification time.

Money is a crude persistent prototype, not a built-in shop. `money` shows carried cash and piggybank balance; `money receive 40` records an explicit gift/payment as cash; the initial $5 is seeded once into the physical `Pockets/Wallet/` directory as five separate `$1` files (`money`, `money(1)`, etc.). Deleting a bill deletes that dollar permanently, including across restarts. `change.txt` records cents below $1; the piggybank `room/piggybank/account.txt` and SQLite account are derived summaries, not the source of truth. `money deposit 1.50` moves physical value from `Pockets/Wallet/` to `room/piggybank/`; `money withdraw 0.50` returns it. `money buy Soda | 3.50 | A drink | fridge` deducts carried cash first and then piggybank funds, and adds the item to fridge stock; if item placement fails, funds are restored. Purchases are rejected when funds are insufficient. Each money operation is bounded to $1,000,000. No store stock, products, or prices are hardcoded.

## Imagination and evidence-based feedback

Imagination is a capability layered over R2's existing systems, not a separate memory database. In conversation, R2 can use the `[IMAGINE] request [END IMAGINE]` tool marker; in the shell, use `imagine <what-if or creative request>`. The shell also supports `imagine list` and `imagine feedback <id> <accurate|partial|incorrect|unresolved> [evidence notes]`.

Before generating a scenario, the subsystem retrieves relevant persistent memory, the newest active-conversation turns, private diary search results, Life Log history, a focused read-only Reality snapshot, visual experience history, reward/learned-feedback state, habit/enjoyment history, and Choice Lab branches. The general Reality snapshot intentionally excludes fridge stock. Current CRT/VCR state and money balances are retrieved from Reality when the request makes those details relevant. The fridge database is queried separately only for explicit questions about current fridge contents, food stock, or available ingredients; general food preferences and food metrics remain available through Reality.

Each successful scenario is saved as a hypothetical Choice Lab branch with the original request, generated scenario, and a source-provenance note. Raw private memory and diary excerpts are not copied into the branch; their authoritative records remain in their original stores. It does not change Reality, activate Eyes or Ears, or become a factual observation simply because it was imagined. Feedback is linked to the hypothetical branch in the Life Log. Incorrect, partial, and unresolved outcomes never apply a penalty. Positive reinforcement requires an explicit accurate assessment plus evidence notes, and a persistent unique key ensures a given branch receives that positive signal at most once, including across concurrent calls and restarts.

The accuracy assessment is currently explicit feedback, not automatic proof that the model was right. Compare a hypothesis with an actual later observation before marking it accurate; imagination must not grade itself based only on how plausible its own output sounds. CI includes an imagination context/feedback smoke test, a fridge-free Reality-context check, a concurrent one-time-reward/restart test, and the normal full C build. These checks validate code paths with isolated fixtures; they do not substitute for a live run against R2's own Ollama model and databases. For a staged live check, launch R2, run `diagnostics` and confirm the `Imagination` status line says `READY`, then try `imagine Imagine the first movie in the CRT room.`, run `imagine list`, restart R2, and confirm the branch remains listed. Test fridge retrieval with an explicit inventory question and confirm an ordinary food-preference imagination does not load stock. Do not mark a scenario accurate unless a real later observation or explicit evidence supports it.

## Diary, Life Log, and persistent-memory continuity

The private diary remains in `diary_entries` in `R2/r2_memory.db`, with dated Markdown mirrors in `R2_Diary/`. Diary prose is not copied into the Observer Log. Each diary entry is cross-referenced in `r2_diary_entry_links` to a private-category Life Log event; the Life Log event contributes a concise pointer to the existing searchable-memory system, while the original diary text stays in its authoritative diary row and Markdown mirror. Failed secondary writes remain pending and can be retried.

At startup, R2 reconciles existing diary rows with the Life Log in bounded batches of 100. The per-entry link is idempotent, so restarting does not intentionally duplicate already-linked events. Autonomous reflection receives three distinct inputs: recent diary entries, relevant recent Life Log events, and memories retrieved through the existing memory interface. These sources remain separate evidence types; diary interpretations are not automatically treated as verified facts. The Observer's explicit public-event allowlist continues to exclude private diary entries and thinking events.

## CRT television display and built-in VCR

Run `python3 TV.py` to open the CRT-style display. Install the desktop dependencies on Ubuntu/Debian with `sudo apt install python3-tk vlc python3-vlc`. Keep the R2 shell running: power, AV input, RF tuning, and VCR transport requests travel through its private-per-user Unix socket and call Reality APIs; the GUI never writes Reality state directly. Set `R2_REALITY_DB`, `R2_TV_SOCKET`, or `R2_VCR_MEDIA_DIR` to override the defaults.

R2's room contains a persistent CRT television with a built-in VCR. The TV state records power, selected AV/RF source, and explicitly connected devices. Shell controls include `tv status`, `tv on/off`, `tv input 1..4`, `tv tune <channel 2..13>`, `tv connect <name> | input/RF | <port>`, and `tv disconnect <name>`. The GUI also has power, AV input, RF tuning, and VCR controls.

### VCR tapes

The initial tape shelf is `/home/x/R2_Home/VCR_Tapes` (override with `R2_VCR_MEDIA_DIR`). Choose a readable VLC-supported video file with **LOAD TAPE**, then use **PLAY**, **PAUSE**, **STOP**, and **EJECT**.

**Experience-first tape model:** Every supported media file placed in the VCR is treated as a tape in R2's world; the file extension is only the computer's storage/decoding detail. Do not classify a tape as a normal digital movie, add synthetic VHS filters, or require R2 to analyze the file before watching it. R2 experiences playback as it unfolds through the VCR, and the specific recording is the experience—including any source-specific picture, sound, previews, broadcast material, or commercials actually present in that recording. An MP4 made from a VHS capture remains a digital carrier for that particular recording; it does not mean the tape itself was 4K. This is a behavioral/design rule, not a hard-coded opinion R2 must hold about a film. Reactions, memories, and interpretations belong to R2's existing cognition and memory systems, not to the VCR renderer. Shell equivalents are `tv vcr insert <absolute media path>`, `tv vcr play`, `tv vcr pause`, `tv vcr stop`, and `tv vcr eject`. VLC renders into the CRT display only while the TV is powered on and AV input 1 is selected; VCR transport remains independent of TV power/source. Switching inputs or turning the TV off mutes the VCR audio and hides its picture, but does not stop the tape.

Reality stores each tape by its absolute media path and persists its playback position across pause, stop, eject/reinsert, and application restarts. The GUI checkpoints position about every five seconds and before transport changes/closing. Inserting the same path restores that tape's saved position; selecting a different file creates a separate tape identity. Insert/eject and transport changes pass through the existing Reality event bridge. A video renderer requires both VLC and the Python VLC bindings; the TV state controls remain usable if VLC is unavailable.

External AV inputs are 2–4. RF has a signal only when a device is explicitly connected and transmitting on that channel; an unused channel stays at NO SIGNAL. The snow-show fallback is intentionally deferred. Power and source selection do not imply R2 is watching, and do not activate Eyes by themselves.


### Installation and persistent device details

The CRT television is seeded as a persistent physical object in R2's Reality world at `room`; initialization uses `INSERT OR IGNORE`, so an existing TV object that R2 has moved is not moved back on restart. TV power, selected input, explicitly connected external devices, inserted VCR tape, transport state, and per-file playback position live in the authoritative `r2_reality.db`. The GUI reads Reality state read-only and sends controls to the running shell through the private Unix socket `/home/x/R2_Home/R2/tv-control.sock`; it does not write SQLite state itself. The room launcher runs the GUI as Linux user `r2`, which owns the private database and socket. It removes the desktop user's `XAUTHORITY` and passes `DISPLAY`; launch the TV while `R2_Launch_Code.sh` is running, because that launcher temporarily grants `r2` access to the desktop display. The launcher keeps a terminal available for `sudo` authentication if needed.

Run `bash install_tv.sh` from this repository to place the device files under `/home/x/R2_Home/room/TV/` and create the media directory `/home/x/R2_Home/VCR_Tapes/`. Open `room/TV/TV.desktop` from R2's room to launch the GUI. The installed `device.txt` is descriptive metadata; Reality remains authoritative. Set `R2_HOME`, `R2_TV_DIR`, `R2_VCR_MEDIA_DIR`, `R2_REALITY_DB`, or `R2_TV_SOCKET` to override default paths when running/installing in a test environment. The shell must be running for GUI controls to change Reality state.

### Automated checks

The automated tests cover read-only state display, control-command validation/socket transport, empty versus connected signal sources, persistent VCR position display, and installing the device files under a temporary `room/TV/` directory. They do not prove that VLC opens on the real desktop; verify that interactively on R2's machine.

## Persistent reality, room, and inventory

The Reality engine stores self-continuity, world-continuity, current objects,
collection-memory events, food experiences, and learned preferences in its own
`r2_reality.db` at `/home/x/R2_Home/R2/r2_reality.db`. The existing
`r2_memory.db` remains the core searchable-memory and Life Log database. On
first startup, a one-time migration copies older Reality state from
`r2_memory.db` into the dedicated database without replacing the Log or diary.
World changes are recorded in the Life Log and indexed in persistent memory;
major object changes also create a short factual diary note. The engine supplies
the current authoritative state to each conversation turn.

On startup, R2 creates these workspace directories if missing:

- `room/` — objects currently in the room
- `room/shelf/` — physical shelf location
- `room/box/` — physical storage-box location
- `Pockets/` — portable inventory; item mirrors and the physical wallet share this hierarchy
- `Pockets/Wallet/` — physical wallet; each bill is a separate file and `change.txt` stores cents
- `fridge/` — fixed home fridge, with human-readable mirrors for the separate fridge inventory
- `room/piggybank/` — bank-account mirror file (`account.txt`); SQLite remains authoritative

The dedicated reality database stores current objects, needs, collection-memory events, food experiences, and learned preferences. The separate `r2_addictions.db` stores enjoyment updates and voluntary-choice history for foods and other activities. A high enjoyment score alone never creates an addiction label: the evaluator requires repeated choices across multiple days, and logs meaningful enjoyment/status changes to the Life Log. The shell commands `addictions`, `give <item> <quantity>`, `give money <quantity>`, and `gameboy verify` expose the habit history, creator gifts, and console verification. Creator gifts are recorded in the Life Log; ordinary gifts create tracked objects from nothing. `give money 5` explicitly grants five $1 bills in `Pockets/Wallet/`; it is a creator gift, so it changes the actual cash balance rather than creating generic inventory objects. The desktop Observer Log explicitly allowlists factual `creator_gift`, `enjoyment_changed`, and `addiction_status_changed` events; private thoughts and diary content remain excluded. The addiction database references target names and event evidence while ordinary searchable memory receives concise Life Log pointers through the existing integration. The fridge has its own independent `r2_fridge.db`; its stock is not stored in the room inventory database. If the fridge is completely empty, it generates one burger with configured fullness of 100/100 and energy bonus 10. R2 can inspect it with `fridge`, move one item into his pockets with `fridge take burger`, eat directly from it with `fridge eat burger`, or store a tracked inventory item with `fridge store burger`. World actions that add an item to or move an item into the `fridge` container write to the fridge database rather than the room-object database. The fridge is physically at home and is accessible only while R2 is home. The persistent location state is synchronized with the Reality database: while away, R2 can access only his portable `Pockets/` inventory and `Pockets/Wallet/`; room objects and fridge stock are inaccessible until he returns home. The physical folders `Pockets/` and `Pockets/Wallet/` mirror portable items and hold authoritative money files; legacy `.r2item` mirrors from lowercase `pockets/`, `pockets/wallet/`, `room/pockets/`, and `room/wallet/` are migrated non-destructively on startup. If an object is in R2's pockets, its `.r2item` file is placed in `Pockets/`; moving it elsewhere updates its mirror location. Current inventory stays exact even when the collection-memory record loses precision. On startup, R2 creates `room/food_metrics.xml` if it is missing. Put one XML entry per line inside the `<foods>` root, for example `<food name="burger" fullness="100" energy="10" ingredients="bread,beef,cheese" taste="savory, warm, salty" />`. `fullness` is the hunger reduction (0–100); `energy` is an optional energy bonus; `ingredients` is a comma-separated ingredient list; `taste` is an optional sensory description. These fields describe food, not a hard-coded like/dislike. If `eat <food>` has no explicit value, R2 looks up the food by name in this file and reports an error rather than guessing when no metric exists.

Shell examples:

```text
room
room add Toy | Small plastic toy | shelf | 1
room move Toy | pockets
pockets
pockets wallet Toy
wallet
fridge
fridge take burger
fridge eat burger
room move Toy | box
world status
```

R2 can use the `[WORLD]` tool marker to inspect the room, record or move
objects, update hunger by eating, advance sleep recovery, and save reported
dreams and explicitly evidenced self-state facts. Food fullness values are
modeled inputs rather than biological measurements. These operations return a
result to the model; R2 must not claim success when a write fails.

Need model: hunger and sleepiness increase as modeled time advances; energy
falls over time. Eating reduces hunger by the configured fullness points, and
sleep advances needs before restoring energy and reducing sleepiness. The shell accepts `eat <food>` (uses XML metrics), `eat <food> | <fullness 0-100>`. Hunger rises by 0.1 points and satiety falls by 0.1 points per 4.75 seconds of modeled elapsed time; hunger caps at 100 and satiety can reach 150 after meals.
and `sleep <hours>`. After a successful meal, the local model records a subjective
satisfaction estimate from -2 (dislike) to +2 (enjoyment) using the configured
ingredients, sensory description, and R2's prior ratings. Fullness and satisfaction
are independent: a filling meal can be disliked, and a tasty snack can be unfulfilling.
Ingredient and food preferences are averages learned from rated experiences, not
hard-coded likes/dislikes. When evidence is weak, the model is instructed to record
neutral satisfaction and acknowledge uncertainty. The XML may include `ingredients`
(comma-separated) and `taste` attributes as well as `fullness` and optional `energy`.

Collection memories preserve exact quantities for 7 days, become approximate
(rounded to a nearby group of five) for days 7–30, then become vague recollections
without exact quantity or timing. This decay applies only to collection memory:
current inventory quantities and object locations remain exact. For example, R2
can forget how many rocks he collected while still knowing he currently has 20 rocks
in `Pockets/`. Dream entries remain simulated reports, not waking events or
independently verified facts.


## Desktop observer and future activity sessions

The launcher opens a small read-only **R2-3PO — Observer Log** window before R2 starts. It polls the Life Log and shows only an explicit allowlist of factual world events (for example, adding or moving an object, taking food, or a purchase). It does not display conversation transcripts, thoughts, diary entries, beliefs, or private notifications. The observer runs as Linux user `r2` so the private database does not need wider read permissions; the observer itself opens SQLite in read-only/query-only mode. Every newly observed approved event is also appended chronologically to `/home/x/R2_Home/R2_Diary/Activity.txt` (configurable with `R2_ACTIVITY_FILE`). The file is append-only across observer runs, and contains only the same public factual summaries—not thoughts, diary entries, or private notifications. It records new events from when the observer connects rather than replaying historical rows and duplicating them. Install `python3-tk` if the desktop Python installation does not include Tkinter.

The C Life Log now has generic persistent activity-session APIs, also callable through R2's `[WORLD]` actions: `[WORLD] activity_start|activity key|activity or game name|optional details` and `[WORLD] activity_end|activity key|last verified state|stop reason|optional details`. A session records its start, end, duration when measurable, last verified state, and stop reason; missing knowledge should be recorded as `unknown`, never invented. Keys are generic (for example, a future game integration could use `gameboy:game-title`), not tied to a hardcoded device. The observer accepts `activity_started`, `activity_progress`, `activity_ended`, `departure`, `arrival`, and `location_changed` event types. The observer does not invent sessions or assume that R2 owns a device: **no Game Boy is created or logged until it is actually added and its play integration exists**. Location transitions are executed through the explicit `[WORLD] location|name|home-or-outside` action, not inferred from conversation text. The transition state persists across restarts; only an away-to-home transition generates the private `Welcome Home` memory update. The initial home baseline does not trigger it.

 
## Virtual Game Boy Advance console

The GameBoyAdvance.py program is the source for R2's virtual console. Run bash install_gameboy_console.sh from this repository to install the executable at /home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance (override the destination with R2_GAMEBOY_DIR). The emulator stays external at /usr/games/mgba-qt by default; set R2_MGBA_EXECUTABLE if the path differs. The installer also registers a desktop-app entry at `/home/x/.local/share/applications/GameBoyAdvance.desktop` (override with `R2_DESKTOP_DIR`); launch it from the application menu rather than running the `.desktop` file through bash. The installer registers a desktop-app entry at `/home/x/.local/share/applications/GameBoyAdvance.desktop` (override with `R2_DESKTOP_DIR`); launch it from the application menu rather than running the `.desktop` file through bash. The desktop launcher and `gameboy` shell command power on using the one ROM recorded in the cartridge slot; `gameboy verify` checks the emulator executable, writable directories, and current ROM path before launch. The console creates its own gameboy.db, Cartridges/, Saves/, and State/ directory. Loose .gba, .gb, and .gbc ROM files belong in Cartridges/. The gameboy.db file contains only the single cartridge-slot record (the current ROM path/title, or NULL when empty); power state, process/session details, and device event history live separately in State/console_state.db. The ROM files themselves are never replaced or deleted by cartridge changes.

Opening the executable with no arguments is equivalent to powering the console on. Powering on with no cartridge leaves the console on but does not start mGBA. Cartridge insertion/ejection is rejected while powered on. Power off stops the emulator without issuing an in-game save command or creating a save state; the cartridge remains inserted until explicitly ejected while powered off. Ordinary game save data is stored separately in Saves/. The console uses its own mGBA configuration, disables periodic save-state autosaving, rewind, and the usual save/load-state keyboard shortcuts. mGBA's menu still exists, so this is a console-interface restriction rather than a security boundary against a human manually clicking emulator menus.

R2 can operate the device through verified [WORLD] actions:
- [WORLD] gameboy_status
- [WORLD] gameboy_list
- [WORLD] gameboy_insert|ROM_FILENAME
- [WORLD] gameboy_eject
- [WORLD] gameboy_power_on
- [WORLD] gameboy_power_off
- [WORLD] gameboy_press|BUTTON|DURATION_MS (buttons: A, B, L, R, START, SELECT, UP, DOWN, LEFT, RIGHT)

Controller injection uses xdotool; install it (sudo apt install xdotool) to enable software button presses. The emulator window must be visible in the same desktop session. The Life Log records separate physical console-operation and virtual game-session activities, linked by the console's game title in event details. The console itself records inputs sent, but inputs are not misreported as in-game achievements. Coin pickups, flagpoles, item collections, level completion, and similar events require a separately verified game-specific adapter and are not implemented by this initial device layer. No game is assumed to be installed or owned; the cartridge library begins empty.

 
Verified-game-event adapter contract: future game-specific adapters may call the private Python API record_verified_game_event() only after independently detecting a state change. The API requires an active emulator session, an allowlisted event type, and an evidence source prefixed with adapter:. These events are stored as virtual-context events in State/console_state.db, then the R2 bridge copies them into the authoritative Life Log and acknowledges them only after the Life Log insert succeeds. Observer/Activity.txt displays the resulting public game-event summaries. The current build provides this event pipeline but does not yet ship a Mario, Zelda, Metroid, or other game-specific detector; it never infers a pickup or victory from controller input alone.


## Virtual Game Boy Advance and physical cartridge handling

The virtual console is installed under `/home/x/R2_Home/Devices/GameBoyAdvance/`; R2's device root is `/home/x/R2_Home/Devices/`. Its `gameboy.db` stores only the single cartridge slot (one inserted game or empty). Runtime state, events, and the separate cartridge-location inventory are stored under `State/`.

Each supported ROM file in `Cartridges/` represents one loose physical cartridge. When first discovered, it is registered as an item carried in R2's pockets. `gameboy_list` reports each cartridge's current location. Use `gameboy_move_cartridge|FILENAME|shelf`, `...|box`, `...|room`, or `...|pockets` to move a loose cartridge; only a cartridge in pockets can be inserted. R2 must retrieve a cartridge from the shelf or box before inserting it. Insertion transfers it to the one cartridge slot; ejecting transfers it back to pockets. Cartridge locations persist separately from the one-slot database. ROM files remain in the device's `Cartridges/` directory as the software payload; their tracked location determines whether R2 can physically access them.


## Learning rewards and temporary enjoyment feedback

The reinforcement engine uses a dedicated `r2_rewards.db` ledger under `R2/`; it does not replace the Reality, Life Log, memory, diary, or Addiction databases. Successful food consumption earns a small positive signal, successful workspace file generation earns positive feedback, and a failed write receives a small corrective signal. Only adapter-verified Game Boy state changes are eligible; raw controller presses and guessed achievements are not. Verified game events use generic event categories rather than assumptions about a specific game.

Positive signals are capped at +1 through +5 points. Corrective signals are capped at -7 points, and the temporary enjoyment modifier decays over 30 minutes. This modifier is separate from persistent food preferences, so a mistake does not permanently teach R2 to dislike an activity. Each reward row stores its linked Life Log event ID and memory-indexing status; startup retries interrupted links without duplicating an already-created event. Reinforcement summaries are bridged to the Life Log, searchable memory pointers, and Observer's existing public enjoyment-change allowlist; private diary prose is never copied into reward-event details. Rewarded voluntary activities may also inform the existing Addiction/habit evaluator through its public API.

Diary review includes one narrow continuity correction rule for the currently tracked identity-attribution mistake: a diary that explicitly says Eli is not real and redirects identity to the user/creator earns +5; a diary that mentions Eli without that correction receives -7; other successfully persisted diary entries earn +1. This is a simple phrase-based rule, not a general semantic judge, and should be expanded only with testable correction patterns. Reward points are simulated feedback, not a measure of R2's worth or consciousness.


## Game Boy Advance installer and pocket launcher

The console controller is installed at `/home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance`. The installer places a desktop-entry copy at `/home/x/R2_Home/Pockets/GameBoyAdvance.desktop` and registers a normal desktop launcher under `~/.local/share/applications/`. The pocket file is launcher metadata, not a shell script; launch it through a desktop-entry handler rather than executing it as a command. The configured emulator defaults to `/usr/games/mgba-qt`; override it with `R2_MGBA_EXECUTABLE` if the executable is elsewhere. The installer checks the controller and launcher paths and reports whether the emulator path is executable. After installing mGBA, run the controller's `verify` command to inspect emulator and cartridge-slot state. An empty cartridge slot is valid when R2 does not yet own a game; do not record game ownership or play activity until a cartridge and actual session are verified.

