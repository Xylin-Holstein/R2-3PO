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

The build requires these seven C translation units:

- `r2.c` — core
- `shell.c` — command shell
- `r2_diary.c` — diary subsystem
- `Log.c` — life log
- `Visual.c` — vision-model integration
- `Ears.c` — audio input
- `Eyes.c` — visual input

Their matching headers are included in the repository: `r2.h`, `shell.h`, `r2_diary.h`, `Log.h`, `Visual.h`, `Ears.h`, and `Eyes.h`.

GitHub Actions checks shell-script syntax and compiles this source set on pushes to the audit branch and the configured main branches. A successful compile confirms compilation/linking only; it does not exercise Ollama, live audio/video devices, or the runtime database.
