CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -D_POSIX_C_SOURCE=200809L \
	$(shell $(PKG_CONFIG) --cflags libcurl libxml-2.0)
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -larchive $(shell $(PKG_CONFIG) --libs libcurl libxml-2.0)

S3_SOURCES = s3_client.c s3_error.c s3_request.c s3_url.c s3_headers.c \
	s3_xml.c s3_retry.c s3_bucket.c s3_object.c s3_list.c s3_put.c \
	s3_config.c s3_uri.c
S3_HEADERS = s3.h s3_internal.h
S3AR_SOURCES = s3ar.c s3ar_create.c s3ar_extract.c s3ar_list.c \
	s3ar_selection.c die.c log.c

.PHONY: all clean test

all: s3ar

s3ar: $(S3AR_SOURCES) s3ar.h die.h log.h $(S3_SOURCES) $(S3_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(S3AR_SOURCES) $(S3_SOURCES) $(LDLIBS)

test: s3ar
	pytest -q

clean:
	rm -f -- s3ar
