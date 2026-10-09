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

You can also compile directly with the same source list and libraries:

```sh
gcc -std=c11 -Wall -Wextra -O2 \
  r2.c shell.c r2_diary.c Log.c Reality.c Addiction.c AlternateSelf.c Visual.c Ears.c Eyes.c \
  -o r2 \
  -lcurl -lsqlite3 -lpthread -ljson-c -lpulse-simple -lpulse -lm
```

## Compilation source inventory

The build requires these ten C translation units:

- `r2.c` — core
- `shell.c` — command shell
- `r2_diary.c` — diary subsystem
- `Log.c` — factual Life Log chronology
- `Reality.c` — persistent self-continuity, world state, room objects, needs, and inventory containers
- `Addiction.c` — persistent enjoyment history and behavior-derived repeated-interest/addiction labels
- `AlternateSelf.c` — persistent, explicitly hypothetical Choice Lab branches
- `Visual.c` — vision-model integration
- `Ears.c` — audio input
- `Eyes.c` — visual input

Their matching headers are included in the repository: `r2.h`, `shell.h`, `r2_diary.h`, `Log.h`, `Reality.h`, `Addiction.h`, `AlternateSelf.h`, `Visual.h`, `Ears.h`, and `Eyes.h`.

GitHub Actions checks shell-script syntax and compiles this source set on pushes to the audit branch and the configured main branches. A successful compile confirms compilation/linking only; it does not exercise Ollama, live audio/video devices, or the runtime database.


## Choice Lab, age, senses, and money

The Choice Lab is open-ended, not an enumerated menu: R2 can consider any proposed choice using current needs, memories, self-facts, and learned preferences. The existing Alternate-Self Lab persists hypothetical branches separately from factual events. R2 can use the shell commands `alternate list`, `alternate show <id>`, `alternate create name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID`, `alternate compare <id> <id>`, `alternate retain <id>`, and `alternate discard <id>`. The model can list/show/compare branches through `[ALTERNATE_LIST]`, `[ALTERNATE_SHOW] id`, and `[ALTERNATE_COMPARE] id id`, and save open-ended alternatives through `[ALTERNATE_CREATE] name|scenario|assumptions|predicted outcome|conclusion|optional evidence event ID [END ALTERNATE_CREATE]`. There is no hardcoded menu of choices. Stored branches remain hypothetical and are separate from factual memories; explicitly stated hypotheticals extracted from responses are imported into the lab without duplicating their Life Log event.

Sensory counterfactuals can ask what a hypothetical view, sound, taste, smell, or touch might reveal without activating Eyes/Ears or changing the world. R2 receives recent Life Log evidence alongside current needs, memories, self-facts, learned food/ingredient preferences, and taste metrics. He must label imagined details as predictions, distinguish observations from guesses, and never update preferences as though an imagined sensation actually occurred. Only real experiences or explicit feedback update learned preferences.

R2's age is measured from the filesystem birth/creation time of `R2/r2_original_conversation.txt` (the original conversation archive), saved in Reality metadata so later edits do not reset it. If the file is absent or the filesystem does not expose a creation timestamp, age is left unestablished rather than guessed from modification time.

Money is a crude persistent prototype, not a built-in shop. `money` shows carried cash and bank balance; `money receive 40` records an explicit gift/payment as cash; the one-time initial $5 balance is seeded separately; `money deposit 30` moves $30 cash into `room/piggybank/`; `money withdraw 10` returns $10 to cash. `money buy Chair | 20 | A chair | room` deducts the price (cash first, then bank) and places the chair in the room; omitting the destination puts a purchased item in pockets so it can be carried to the fridge or elsewhere. A one-time initial $5 cash balance is seeded into carried money and mirrored in `pockets/wallet/cash.txt`; this migration is tracked in Reality metadata so restarts never grant it again. No store stock, products, or prices are hardcoded.

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
- `pockets/` — portable inventory; items remain accessible wherever R2 goes
- `pockets/wallet/` — portable wallet, including the mirrored carried-cash balance in `cash.txt`
- `fridge/` — fixed home fridge, with human-readable mirrors for the separate fridge inventory
- `room/piggybank/` — bank-account mirror file (`account.txt`); SQLite remains authoritative

The dedicated reality database stores current objects, needs, collection-memory events, food experiences, and learned preferences. The separate `r2_addictions.db` stores enjoyment updates and voluntary-choice history for foods and other activities. A high enjoyment score alone never creates an addiction label: the evaluator requires repeated choices across multiple days, and logs meaningful enjoyment/status changes to the Life Log. The shell command `addictions` shows the current recorded status. The desktop Observer Log explicitly allowlists the factual event types `enjoyment_changed` and `addiction_status_changed`; private thoughts and diary content remain excluded. The addiction database references target names and event evidence while ordinary searchable memory receives concise Life Log pointers through the existing integration. The fridge has its own independent `r2_fridge.db`; its stock is not stored in the room inventory database. If the fridge is completely empty, it generates one burger with configured fullness of 100/100 and energy bonus 10. R2 can inspect it with `fridge`, move one item into his pockets with `fridge take burger`, eat directly from it with `fridge eat burger`, or store a tracked inventory item with `fridge store burger`. World actions that add an item to or move an item into the `fridge` container write to the fridge database rather than the room-object database. The fridge is physically at home and is accessible only while R2 is home. The persistent location state is synchronized with the Reality database: while away, R2 can access only his portable `pockets/` and `pockets/wallet/`; room objects and fridge stock are inaccessible until he returns home. The physical folders `pockets/` and `pockets/wallet/` mirror carried items and wallet cash; the old `room/pockets/`, `room/wallet/`, and `room/toy_box/` mirrors are migrated non-destructively on startup. If an object is in R2's pockets, its `.r2item` file is placed in `pockets/`; moving it elsewhere updates its mirror location. Current inventory stays exact even when the collection-memory record loses precision. On startup, R2 creates `room/food_metrics.xml` if it is missing. Put one XML entry per line inside the `<foods>` root, for example `<food name="burger" fullness="100" energy="10" ingredients="bread,beef,cheese" taste="savory, warm, salty" />`. `fullness` is the hunger reduction (0–100); `energy` is an optional energy bonus; `ingredients` is a comma-separated ingredient list; `taste` is an optional sensory description. These fields describe food, not a hard-coded like/dislike. If `eat <food>` has no explicit value, R2 looks up the food by name in this file and reports an error rather than guessing when no metric exists.

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
sleep advances needs before restoring energy and reducing sleepiness. The shell accepts `eat <food>` (uses XML metrics), `eat <food> | <fullness 0-100>`,
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
in `pockets/`. Dream entries remain simulated reports, not waking events or
independently verified facts.


## Desktop observer and future activity sessions

The launcher opens a small read-only **R2-3PO — Observer Log** window before R2 starts. It polls the Life Log and shows only an explicit allowlist of factual world events (for example, adding or moving an object, taking food, or a purchase). It does not display conversation transcripts, thoughts, diary entries, beliefs, or private notifications. The observer runs as Linux user `r2` so the private database does not need wider read permissions; the observer itself opens SQLite in read-only/query-only mode. Every newly observed approved event is also appended chronologically to `/home/x/R2_Home/R2_Diary/Activity.txt` (configurable with `R2_ACTIVITY_FILE`). The file is append-only across observer runs, and contains only the same public factual summaries—not thoughts, diary entries, or private notifications. It records new events from when the observer connects rather than replaying historical rows and duplicating them. Install `python3-tk` if the desktop Python installation does not include Tkinter.

The C Life Log now has generic persistent activity-session APIs, also callable through R2's `[WORLD]` actions: `[WORLD] activity_start|activity key|activity or game name|optional details` and `[WORLD] activity_end|activity key|last verified state|stop reason|optional details`. A session records its start, end, duration when measurable, last verified state, and stop reason; missing knowledge should be recorded as `unknown`, never invented. Keys are generic (for example, a future game integration could use `gameboy:game-title`), not tied to a hardcoded device. The observer accepts `activity_started`, `activity_progress`, `activity_ended`, `departure`, `arrival`, and `location_changed` event types. The observer does not invent sessions or assume that R2 owns a device: **no Game Boy is created or logged until it is actually added and its play integration exists**. Location transitions are executed through the explicit `[WORLD] location|name|home-or-outside` action, not inferred from conversation text. The transition state persists across restarts; only an away-to-home transition generates the private `Welcome Home` memory update. The initial home baseline does not trigger it.

 
## Virtual Game Boy Advance console

The GameBoyAdvance.py program is the source for R2's virtual console. Run bash install_gameboy_console.sh from this repository to install the executable at /home/x/R2_Home/Devices/GameBoyAdvance/GameBoyAdvance (override the destination with R2_GAMEBOY_DIR). The emulator stays external at /usr/games/mgba-qt by default; set R2_MGBA_EXECUTABLE if the path differs. The console creates its own gameboy.db, Cartridges/, Saves/, and State/ directory. Loose .gba, .gb, and .gbc ROM files belong in Cartridges/. The gameboy.db file contains only the single cartridge-slot record (the current ROM path/title, or NULL when empty); power state, process/session details, and device event history live separately in State/console_state.db. The ROM files themselves are never replaced or deleted by cartridge changes.

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
