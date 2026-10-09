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
  r2.c shell.c r2_diary.c Log.c Visual.c Ears.c Eyes.c \
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

The Reality engine stores self-continuity and world-continuity in separate
`r2_reality_*` tables in the existing `r2_memory.db`. It shares the database
file with the core memory, diary, and Life Log without replacing their tables.
World changes are recorded in the Life Log and indexed in persistent memory;
major object changes also create a short factual diary note. The engine supplies
the current authoritative state to each conversation turn.

On startup, R2 creates these workspace directories if missing:

- `room/` — objects currently in the room
- `room/shelf/` — physical shelf location
- `room/box/` — physical storage-box location

The database also provides `pockets` and `wallet` inventory containers.

Shell examples:

```text
room
room add Toy | Small plastic toy | shelf | 1
room move Toy | pockets
pockets
pockets wallet Toy
wallet
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
sleep advances needs before restoring energy and reducing sleepiness. The current
shell accepts explicit modeled values with `eat <food> | <fullness 0-100>` and
`sleep <hours>`. Dream entries are stored as reports, not independently verified
facts.
