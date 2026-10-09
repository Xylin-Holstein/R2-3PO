# R2-3PO

R2-3PO is a C program with separate core, shell, diary, life-log, visual, eye, hearing, and filesystem-backed personal-world modules.

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

The build requires these seven C translation units:

- `r2.c` — core
- `shell.c` — command shell
- `r2_diary.c` — diary subsystem
- `Log.c` — life log
- `Visual.c` — vision-model integration
- `Ears.c` — audio input
- `Eyes.c` — visual input\n- `World.c` — Pockets, Wallet, Room, shelf/box storage, object counting, and safe possession movement

Their matching headers are included in the repository: `r2.h`, `shell.h`, `r2_diary.h`, `Log.h`, `Visual.h`, `Ears.h`, and `Eyes.h`.

GitHub Actions checks shell-script syntax and compiles this source set on pushes to the audit branch and the configured main branches. A successful compile confirms compilation/linking only; it does not exercise Ollama, live audio/video devices, or the runtime database.


## R2's filesystem-backed personal world

The launcher creates these persistent directories without deleting existing contents:

- `Pockets/` — carried possessions
- `Pockets/Wallet/` — one-dollar files named `money`, `money(1)`, etc.
- `Room/` — stored possessions, accessible to R2 after entering his room
- `Room/shelf/` and `Room/box/` — optional storage locations

A hidden wallet marker makes the initial five-dollar grant a one-time initialization; relaunching R2 does not replenish money. The `World.c` tool enforces location-aware access and supports inspection, counting, wallet checks, entering/leaving the room, and moving regular object files without overwriting existing items. Ordinary files remain valid possessions without custom code; specialized behavior can be added later as explicit capabilities.
