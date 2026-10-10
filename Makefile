CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
LDLIBS ?= -lcurl -lsqlite3 -lpthread -ljson-c -lpulse-simple -lpulse -lm

SOURCES = r2.c shell.c r2_diary.c Log.c Reality.c Addiction.c Reward.c AlternateSelf.c Visual.c Ears.c Eyes.c R2Sounds.c
HEADERS = r2.h shell.h r2_diary.h Log.h Reality.h Addiction.h Reward.h AlternateSelf.h Visual.h Ears.h Eyes.h R2Sounds.h
TARGET ?= r2

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
