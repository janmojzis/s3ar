CC ?= cc
.DEFAULT_GOAL := all
AR ?= ar
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

LIBXML_CPPFLAGS ?= -I/usr/include/libxml2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -I. $(LIBXML_CPPFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic
S3_LIBS ?= -lcurl -lxml2 -lcrypto -lrandombytes
ARCHIVE_LIBS ?= -larchive

S3_SOURCES = s3_client.c s3_config.c s3_error.c s3_log.c s3_result.c s3_memory.c s3_request.c s3_response.c s3_object_properties.c s3_url.c s3_uri_encode.c \
	s3_headers.c s3_xml.c s3_retry.c s3_trace.c s3_bucket.c s3_bucket_list.c s3_bucket_acl.c \
	s3_object.c s3_object_list.c s3_object_put.c s3_object_delete.c s3_listing.c s3_multipart.c s3_uri.c
S3AR_SOURCES = main_s3ar.c s3ar_log.c s3ar_config.c s3ar_interrupt.c s3ar_create.c s3ar_extract.c s3ar_selection.c \
	s3ar_transform.c fsyncfile.c
S3_OBJECTS = $(S3_SOURCES:.c=.o)
S3AR_OBJECTS = $(S3AR_SOURCES:.c=.o)
TOOL_OBJECTS = main_s3ar_put.o main_s3ar_get.o main_s3ar_delete.o main_s3ar_list.o main_s3ar_copy.o \
	s3ar_io.o
PROGRAM_OBJECTS = main.o $(S3AR_OBJECTS) $(TOOL_OBJECTS) sig.o
LINKS = s3ar-put s3ar-get s3ar-delete s3ar-list s3ar-copy
TEST_ERROR_OBJECTS = tests/test_error.o
TEST_GET_RETRY_OBJECTS = tests/get_retry_probe.o
TEST_HEADERS_ALLOC_OBJECTS = tests/test_headers_alloc.o
TEST_URI_ENCODE_OBJECTS = tests/test_uri_encode.o
TEST_LOG_SIGNAL_OBJECTS = tests/test_log_signal.o
TEST_LOG_ESCAPE_OBJECTS = tests/test_log_escape.o
TEST_DELETE_BATCH_OBJECTS = tests/test_delete_batch.o
TEST_SIGNAL_IO_OBJECTS = tests/test_signal_io.o
TEST_PUT_CANCEL_OBJECTS = tests/test_put_cancel.o
TEST_SELECTION_OBJECTS = tests/test_selection.o
TEST_TRANSFORM_OBJECTS = tests/test_transform.o
TEST_TRANSFORM_RESTORE_OBJECTS = tests/transform_restore_probe.o
TEST_OBJECTS = $(TEST_ERROR_OBJECTS) $(TEST_GET_RETRY_OBJECTS) \
	$(TEST_HEADERS_ALLOC_OBJECTS) $(TEST_URI_ENCODE_OBJECTS) \
	$(TEST_LOG_SIGNAL_OBJECTS) $(TEST_LOG_ESCAPE_OBJECTS) $(TEST_DELETE_BATCH_OBJECTS) $(TEST_SIGNAL_IO_OBJECTS) \
	$(TEST_PUT_CANCEL_OBJECTS) $(TEST_SELECTION_OBJECTS) $(TEST_TRANSFORM_OBJECTS) $(TEST_TRANSFORM_RESTORE_OBJECTS)
OBJECTS = $(PROGRAM_OBJECTS) $(S3_OBJECTS) log.o $(TEST_OBJECTS)
DEPENDENCIES = $(OBJECTS:.o=.d)
C_SOURCES = $(S3_SOURCES) $(S3AR_SOURCES) main.c main_s3ar_put.c main_s3ar_get.c \
	s3ar_io.c log.c sig.c main_s3ar_delete.c main_s3ar_list.c main_s3ar_copy.c \
	tests/test_error.c tests/get_retry_probe.c tests/test_headers_alloc.c \
	tests/test_uri_encode.c tests/test_log_signal.c tests/test_log_escape.c tests/test_delete_batch.c \
	tests/test_signal_io.c tests/test_put_cancel.c tests/test_selection.c tests/test_transform.c \
	tests/transform_restore_probe.c
PUBLIC_HEADERS = s3.h s3_log.h log.h

.PHONY: all clean format-check install test install-libs

all: s3ar libs3.a liblog.a $(LINKS)

libs3.a: $(S3_OBJECTS)
	$(RM) $@
	$(AR) rcs $@ $^

liblog.a: log.o
	$(AR) rcs $@ $^

s3ar: $(PROGRAM_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -o $@ $(PROGRAM_OBJECTS) libs3.a liblog.a \
		$(ARCHIVE_LIBS) $(S3_LIBS)

$(LINKS): s3ar
	ln -sfn s3ar $@

test-error: $(TEST_ERROR_OBJECTS) libs3.a liblog.a s3ar_config.o s3ar_log.o
	$(CC) $(LDFLAGS) -o $@ $(TEST_ERROR_OBJECTS) s3ar_config.o s3ar_log.o libs3.a liblog.a $(S3_LIBS)

test-get-retry: $(TEST_GET_RETRY_OBJECTS) libs3.a liblog.a s3ar_config.o s3ar_log.o
	$(CC) $(LDFLAGS) -o $@ $(TEST_GET_RETRY_OBJECTS) s3ar_config.o s3ar_log.o libs3.a liblog.a \
		$(S3_LIBS)

test-headers-alloc: $(TEST_HEADERS_ALLOC_OBJECTS) s3_headers.o s3_response.o s3_object_properties.o
	$(CC) $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@ \
		$(TEST_HEADERS_ALLOC_OBJECTS) s3_headers.o s3_response.o s3_object_properties.o $(S3_LIBS)

test-uri-encode: $(TEST_URI_ENCODE_OBJECTS) s3_uri_encode.o
	$(CC) $(LDFLAGS) -o $@ $(TEST_URI_ENCODE_OBJECTS) s3_uri_encode.o

test-log-signal: $(TEST_LOG_SIGNAL_OBJECTS) liblog.a sig.o
	$(CC) $(LDFLAGS) -o $@ $(TEST_LOG_SIGNAL_OBJECTS) liblog.a sig.o

test-log-escape: $(TEST_LOG_ESCAPE_OBJECTS) liblog.a
	$(CC) $(LDFLAGS) -o $@ $(TEST_LOG_ESCAPE_OBJECTS) liblog.a

test-selection: $(TEST_SELECTION_OBJECTS) s3ar_selection.o s3ar_log.o libs3.a liblog.a
	$(CC) $(LDFLAGS) -o $@ $^ $(S3_LIBS)

test-transform: $(TEST_TRANSFORM_OBJECTS) s3ar_transform.o liblog.a
	$(CC) $(LDFLAGS) -o $@ $(TEST_TRANSFORM_OBJECTS) s3ar_transform.o liblog.a

test-transform-restore: $(TEST_TRANSFORM_RESTORE_OBJECTS) $(filter-out main.o,$(PROGRAM_OBJECTS)) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_bucket_ensure -Wl,--wrap=s3_object_put -o $@ \
		$(TEST_TRANSFORM_RESTORE_OBJECTS) $(filter-out main.o,$(PROGRAM_OBJECTS)) libs3.a liblog.a \
		$(ARCHIVE_LIBS) $(S3_LIBS)

test-delete-batch: $(TEST_DELETE_BATCH_OBJECTS) $(filter-out s3_object_delete.o,$(S3_OBJECTS)) log.o
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_request_bucket -Wl,--wrap=s3_request_url -o $@ $(TEST_DELETE_BATCH_OBJECTS) $(filter-out s3_object_delete.o,$(S3_OBJECTS)) log.o $(S3_LIBS)

test-signal-io: $(TEST_SIGNAL_IO_OBJECTS) s3ar_io.o sig.o
	$(CC) $(LDFLAGS) -o $@ $(TEST_SIGNAL_IO_OBJECTS) s3ar_io.o sig.o

test-put-cancel: $(TEST_PUT_CANCEL_OBJECTS) libs3.a liblog.a
	$(CC) $(LDFLAGS) -Wl,--wrap=s3_response_memory_reset \
		-Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo \
		-Wl,--wrap=malloc -o $@ \
		$(TEST_PUT_CANCEL_OBJECTS) libs3.a liblog.a $(S3_LIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

TEST_PROGRAMS ?= all

test: CFLAGS += -Werror
test: $(TEST_PROGRAMS) test-error test-get-retry test-headers-alloc test-uri-encode test-log-signal test-log-escape test-delete-batch test-signal-io test-put-cancel test-selection test-transform test-transform-restore
	./test-error
	./test-headers-alloc
	./test-uri-encode
	./test-log-signal
	./test-log-escape
	./test-delete-batch
	./test-signal-io
	./test-put-cancel
	./test-selection
	./test-transform
	pytest -q

format-check:
	clang-format --dry-run --Werror $(C_SOURCES) $(PUBLIC_HEADERS) \
		s3_internal.h s3_xml.h s3ar.h s3ar_io.h s3ar_config.h \
		s3ar_log.h s3ar_interrupt.h s3ar_transform.h s3ar_selection.h fsyncfile.h main.h sig.h

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
	rm -f -- s3ar $(LINKS) libs3.a liblog.a test-error test-get-retry test-headers-alloc test-uri-encode test-log-signal test-log-escape test-delete-batch test-signal-io test-put-cancel test-selection test-transform test-transform-restore $(OBJECTS) \
		$(DEPENDENCIES)
	rm -rf -- __pycache__ tests/__pycache__ .pytest_cache

-include $(DEPENDENCIES)
