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
  r2.c shell.c r2_diary.c Log.c Reality.c Visual.c Ears.c Eyes.c \
  -o r2 \
  -lcurl -lsqlite3 -lpthread -ljson-c -lpulse-simple -lpulse -lm
```

## Compilation source inventory

The build requires these eight C translation units:

- `r2.c` — core
- `shell.c` — command shell
- `r2_diary.c` — diary subsystem
- `Log.c` — factual Life Log chronology
- `Reality.c` — persistent self-continuity, world state, room objects, needs, and inventory containers
- `Visual.c` — vision-model integration
- `Ears.c` — audio input
- `Eyes.c` — visual input

Their matching headers are included in the repository: `r2.h`, `shell.h`, `r2_diary.h`, `Log.h`, `Reality.h`, `Visual.h`, `Ears.h`, and `Eyes.h`.

GitHub Actions checks shell-script syntax and compiles this source set on pushes to the audit branch and the configured main branches. A successful compile confirms compilation/linking only; it does not exercise Ollama, live audio/video devices, or the runtime database.


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
- `room/pockets/` and `room/wallet/` — physical inventory folders for carried items
- `room/toy_box/` — the named toy-storage container
- `room/fridge/` — human-readable mirror files for the separate fridge inventory

The dedicated reality database stores current objects, needs, collection-memory events, food experiences, and learned preferences. The fridge has its own independent `r2_fridge.db`; its stock is not stored in the room inventory database. If the fridge is completely empty, it generates one burger with configured fullness of 100/100 and energy bonus 10. R2 can inspect it with `fridge`, move one item into his pockets with `fridge take burger`, eat directly from it with `fridge eat burger`, or store a tracked inventory item with `fridge store burger`. World actions that add an item to or move an item into the `fridge` container write to the fridge database rather than the room-object database. In this prototype the fridge can be interacted with from anywhere; a later world-layout pass can restrict it to the kitchen. The physical folders `room/pockets/`, `room/wallet/`, and `room/toy_box/` mirror tracked items in those containers. If an object is in R2's pockets, its `.r2item` file is placed in `room/pockets/`; moving it elsewhere updates its mirror location. Current inventory stays exact even when the collection-memory record loses precision. On startup, R2 creates `room/food_metrics.xml` if it is missing. Put one XML entry per line inside the `<foods>` root, for example `<food name="burger" fullness="100" energy="10" ingredients="bread,beef,cheese" taste="savory, warm, salty" />`. `fullness` is the hunger reduction (0–100); `energy` is an optional energy bonus; `ingredients` is a comma-separated ingredient list; `taste` is an optional sensory description. These fields describe food, not a hard-coded like/dislike. If `eat <food>` has no explicit value, R2 looks up the food by name in this file and reports an error rather than guessing when no metric exists.

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
in `room/pockets/`. Dream entries remain simulated reports, not waking events or
independently verified facts.
