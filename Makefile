CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -ls3 -larchive

.PHONY: all clean

all: s3ar

s3ar: s3ar.c s3ar.h s3ar_create.c s3ar_list.c s3ar_selection.c s3.c s3.h die.c die.h log.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ s3ar.c s3ar_create.c s3ar_list.c s3ar_selection.c s3.c die.c log.c $(LDLIBS)

clean:
	rm -f -- s3ar
