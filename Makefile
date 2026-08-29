CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -ls3

.PHONY: all clean

all: s3ar

s3ar: s3ar.c s3ar.h list.c s3.c s3.h die.c die.h log.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ s3ar.c list.c s3.c die.c log.c $(LDLIBS)

clean:
	rm -f -- s3ar
