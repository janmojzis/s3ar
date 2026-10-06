CC ?= cc
.DEFAULT_GOAL := all
AR ?= ar
NM ?= nm
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
PANDOC ?= pandoc
PANDOC_VERSION ?= 3.1.11.1
# Markdown is the source of truth for all installed manuals.
MAN_MARKDOWN = s3ar.md s3ar-copy.md s3ar-delete.md s3ar-get.md s3ar-list.md s3ar-put.md

LIBXML_CPPFLAGS ?= -I/usr/include/libxml2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I. $(LIBXML_CPPFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic
S3_LIBS ?= -lcurl -lxml2 -lcrypto -lrandombytes
ARCHIVE_LIBS ?= -larchive
HASH_CPPFLAGS ?=
HASH_LIBS ?= -lnettle
CPPFLAGS += $(HASH_CPPFLAGS)

S3_SOURCES = s3_client.c s3_config.c s3_error.c s3_log.c s3_result.c s3_memory.c s3_request.c s3_response.c s3_parse.c s3_object_properties.c s3_url.c s3_uri_encode.c s3_uri_decode.c s3_query.c \
	s3_headers.c s3_xml.c s3_retry.c s3_trace.c s3_bucket.c s3_bucket_list.c s3_bucket_acl.c \
	s3_object.c s3_object_list.c s3_object_put.c s3_object_copy.c s3_upload.c s3_object_delete.c s3_listing.c s3_multipart.c s3_uri.c secure_free.c
S3AR_SOURCES = main_s3ar.c s3ar.c s3ar_log.c s3ar_config.c s3ar_parse.c s3ar_client.c s3ar_interrupt.c s3ar_hash.c s3ar_xattr.c s3ar_create.c s3ar_archive_reader.c s3ar_selection.c \
	s3ar_transform.c fsyncfile.c
S3_OBJECTS = $(S3_SOURCES:.c=.o)
S3AR_OBJECTS = $(S3AR_SOURCES:.c=.o)
TOOL_OBJECTS = main_s3ar_put.o main_s3ar_get.o main_s3ar_delete.o main_s3ar_list.o main_s3ar_copy.o \
	s3ar_io.o
PROGRAM_OBJECTS = main.o $(S3AR_OBJECTS) $(TOOL_OBJECTS) sig.o
LINKS = s3ar-put s3ar-get s3ar-delete s3ar-list s3ar-copy
# Tests run directly; probes are driven by the Python tests.
C_TESTS = test-list-reentrancy test-request-headers test-error test-headers-alloc \
	test-uri-encode test-log-signal test-log-escape test-delete-batch \
	test-signal-io test-put-cancel test-selection test-transform test-create-hash
C_TEST_PROBES = test-get-retry test-transform-restore
C_TEST_PROGRAMS = $(C_TESTS) $(C_TEST_PROBES)
test-list-reentrancy_OBJECTS = tests/test_list_reentrancy.o
test-request-headers_OBJECTS = tests/test_request_headers.o
test-error_OBJECTS = tests/test_error.o
test-get-retry_OBJECTS = tests/get_retry_probe.o
test-headers-alloc_OBJECTS = tests/test_headers_alloc.o
test-uri-encode_OBJECTS = tests/test_uri_encode.o
test-log-signal_OBJECTS = tests/test_log_signal.o
test-log-escape_OBJECTS = tests/test_log_escape.o
test-delete-batch_OBJECTS = tests/test_delete_batch.o
test-signal-io_OBJECTS = tests/test_signal_io.o
test-put-cancel_OBJECTS = tests/test_put_cancel.o
test-selection_OBJECTS = tests/test_selection.o
test-transform_OBJECTS = tests/test_transform.o
test-transform-restore_OBJECTS = tests/transform_restore_probe.o
test-create-hash_OBJECTS = tests/test_create_hash.o
TEST_OBJECTS = $(foreach target,$(C_TEST_PROGRAMS),$($(target)_OBJECTS))
OBJECTS = $(PROGRAM_OBJECTS) $(S3_OBJECTS) log.o $(TEST_OBJECTS)
DEPENDENCIES = $(OBJECTS:.o=.d)
C_SOURCES = $(sort $(OBJECTS:.o=.c))
PUBLIC_HEADERS = s3.h s3_log.h log.h

.PHONY: all clean format-check install test install-libs man check-man check-symbols

all: s3ar libs3.a liblog.a $(LINKS)

libs3.a: $(S3_OBJECTS)
	$(RM) $@
	$(AR) rcs $@ $^

liblog.a: log.o
	$(AR) rcs $@ $^

s3ar: $(PROGRAM_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) libs3.a liblog.a \
		$(ARCHIVE_LIBS) $(HASH_LIBS) $(S3_LIBS)

$(LINKS): s3ar
	ln -sfn s3ar $@

test-list-reentrancy: $(test-list-reentrancy_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_response_memory_reset \
		-Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo \
		-o $@ $^ $(S3_LIBS)

test-request-headers: $(test-request-headers_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_response_memory_reset \
		-Wl,--wrap=s3_response_reset \
		-Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo \
		-Wl,--wrap=malloc -o $@ $^ $(S3_LIBS)

test-error: $(test-error_OBJECTS) libs3.a liblog.a s3ar_config.o s3ar_parse.o s3ar_log.o
	$(CC) $(LDFLAGS) -o $@ $(test-error_OBJECTS) s3ar_config.o s3ar_parse.o s3ar_log.o libs3.a liblog.a $(S3_LIBS)

test-get-retry: $(test-get-retry_OBJECTS) libs3.a liblog.a s3ar_config.o s3ar_log.o
	$(CC) $(LDFLAGS) -o $@ $(test-get-retry_OBJECTS) s3ar_config.o s3ar_log.o libs3.a liblog.a \
		$(S3_LIBS)

test-headers-alloc: $(test-headers-alloc_OBJECTS) s3_headers.o s3_response.o s3_parse.o s3_object_properties.o s3_memory.o s3_error.o s3_xml.o
	$(CC) $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@ \
		$(test-headers-alloc_OBJECTS) s3_headers.o s3_response.o s3_parse.o s3_object_properties.o s3_memory.o s3_error.o s3_xml.o $(S3_LIBS)

test-uri-encode: $(test-uri-encode_OBJECTS) s3_uri_encode.o s3_uri_decode.o s3_query.o
	$(CC) $(LDFLAGS) -o $@ $(test-uri-encode_OBJECTS) s3_uri_encode.o s3_uri_decode.o s3_query.o

test-log-signal: $(test-log-signal_OBJECTS) liblog.a sig.o
	$(CC) $(LDFLAGS) -o $@ $(test-log-signal_OBJECTS) liblog.a sig.o

test-log-escape: $(test-log-escape_OBJECTS) liblog.a
	$(CC) $(LDFLAGS) -o $@ $(test-log-escape_OBJECTS) liblog.a

test-selection: $(test-selection_OBJECTS) s3ar_selection.o s3ar_log.o libs3.a liblog.a
	$(CC) $(LDFLAGS) -o $@ $^ $(S3_LIBS)

test-transform: $(test-transform_OBJECTS) s3ar_transform.o liblog.a
	$(CC) $(LDFLAGS) -o $@ $(test-transform_OBJECTS) s3ar_transform.o liblog.a

test-create-hash: $(test-create-hash_OBJECTS) $(filter-out main.o s3ar_create.o,$(PROGRAM_OBJECTS)) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=s3_object_get -o $@ $^ $(ARCHIVE_LIBS) $(HASH_LIBS) $(S3_LIBS)

test-transform-restore: $(test-transform-restore_OBJECTS) $(filter-out main.o,$(PROGRAM_OBJECTS)) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_bucket_ensure -Wl,--wrap=s3_object_put -o $@ \
		$(test-transform-restore_OBJECTS) $(filter-out main.o,$(PROGRAM_OBJECTS)) libs3.a liblog.a \
		$(ARCHIVE_LIBS) $(HASH_LIBS) $(S3_LIBS)

test-delete-batch: $(test-delete-batch_OBJECTS) $(filter-out s3_object_delete.o,$(S3_OBJECTS)) log.o
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_request_bucket -Wl,--wrap=s3_request_url -o $@ $(test-delete-batch_OBJECTS) $(filter-out s3_object_delete.o,$(S3_OBJECTS)) log.o $(S3_LIBS)

test-signal-io: $(test-signal-io_OBJECTS) s3ar_io.o sig.o
	$(CC) $(LDFLAGS) -o $@ $(test-signal-io_OBJECTS) s3ar_io.o sig.o

test-put-cancel: $(test-put-cancel_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_response_memory_reset \
		-Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo \
		-Wl,--wrap=malloc -o $@ \
		$(test-put-cancel_OBJECTS) libs3.a liblog.a $(S3_LIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

TEST_PROGRAMS ?= all

check-symbols: libs3.a liblog.a
	NM="$(NM)" python3 tests/check_archive_symbols.py libs3.a liblog.a

test: CFLAGS += -Werror
test: $(C_TEST_PROGRAMS) $(TEST_PROGRAMS) check-symbols
	@set -e; for program in $(C_TESTS); do ./$$program; done
	pytest -q

format-check:
	clang-format --dry-run --Werror $(C_SOURCES) $(PUBLIC_HEADERS) \
		s3_internal.h s3_xml.h s3_upload.h s3ar.h s3ar_io.h s3ar_config.h s3ar_parse.h s3ar_client.h \
		s3ar_log.h s3ar_hash.h s3ar_format.h s3ar_xattr.h s3ar_interrupt.h s3ar_transform.h s3ar_selection.h s3ar_archive_reader.h fsyncfile.h main.h sig.h secure_free.h

# Kept separate from all/install so building from Git needs no Pandoc.
man:
	PANDOC="$(PANDOC)" PANDOC_VERSION="$(PANDOC_VERSION)" sh scripts/generate-man.sh build $(MAN_MARKDOWN)

check-man:
	PANDOC="$(PANDOC)" PANDOC_VERSION="$(PANDOC_VERSION)" sh scripts/generate-man.sh check $(MAN_MARKDOWN)

install-libs: libs3.a liblog.a
	install -d $(DESTDIR)$(LIBDIR)/s3ar \
		$(DESTDIR)$(PREFIX)/include/s3ar
	install -m 0644 libs3.a liblog.a $(DESTDIR)$(LIBDIR)/s3ar/
	install -m 0644 $(PUBLIC_HEADERS) $(DESTDIR)$(PREFIX)/include/s3ar/

install: all install-libs
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 s3ar $(DESTDIR)$(PREFIX)/bin/s3ar
	for link in $(LINKS); do \
		ln -sfn s3ar $(DESTDIR)$(PREFIX)/bin/$$link; \
	done
	install -m 0644 s3ar.1 s3ar-put.1 s3ar-get.1 s3ar-delete.1 s3ar-list.1 s3ar-copy.1 \
		$(DESTDIR)$(PREFIX)/share/man/man1/

clean:
	rm -f -- $(C_TEST_PROGRAMS) s3ar $(LINKS) libs3.a liblog.a $(OBJECTS) \
		$(DEPENDENCIES)
	rm -rf -- __pycache__ tests/__pycache__ .pytest_cache

-include $(DEPENDENCIES)
