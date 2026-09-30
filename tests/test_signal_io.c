/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_io.h"
#include "s3ar_io.h"
#include "sig.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void record_signal(int signal_number) { interrupted = signal_number; }

int main(void) {
    struct sigaction action;
    unsigned char buffer[4];
    size_t size = 99;
    int pipe_fds[2];

    sig_catch(SIGUSR1, record_signal);
    assert(sigaction(SIGUSR1, NULL, &action) == 0);
    assert((action.sa_flags & SA_RESTART) == 0);
    assert(raise(SIGUSR1) == 0);
    assert(interrupted == SIGUSR1);

    assert(pipe(pipe_fds) == 0);
    struct s3ar_io_read_context input = {.fd = &pipe_fds[0],
                                         .interrupted = &interrupted};
    assert(s3ar_io_read(&input, buffer, sizeof(buffer), &size) ==
           S3_READ_ERROR);
    assert(size == 0 && errno == EINTR);

    interrupted = 0;
    sig_catch(SIGALRM, record_signal);
    assert(alarm(1) == 0);
    assert(s3ar_io_read(&input, buffer, sizeof(buffer), &size) ==
           S3_READ_ERROR);
    assert(interrupted == SIGALRM && errno == EINTR && size == 0);

    struct s3ar_io_write_context output = {.fd = &pipe_fds[1],
                                           .interrupted = &interrupted};
    assert(!s3ar_io_write(&output, buffer, sizeof(buffer)));
    assert(errno == EINTR);
    assert(close(pipe_fds[0]) == 0);
    assert(close(pipe_fds[1]) == 0);
    return 0;
}
