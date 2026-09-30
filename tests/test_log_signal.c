/* SPDX-License-Identifier: MIT-0 */
#include "log.h"
#include "sig.h"

#include <assert.h>
#include <signal.h>

int main(void) {
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    assert(log_enabled(log_OUTPUT));
    assert(log_enabled(log_SUCCESS));
    assert(log_enabled(log_FATAL));
    assert(log_enabled(log_WARNING));
    assert(!log_enabled(log_ERROR));
    assert(!log_enabled(log_INFO));
    assert(raise(SIGUSR1) == 0);
    assert(log_enabled(log_ERROR));
    assert(log_enabled(log_WARNING));
    assert(log_enabled(log_INFO));
    assert(!log_enabled(log_DEBUG));
    assert(raise(SIGUSR1) == 0);
    assert(log_enabled(log_DEBUG));
    assert(!log_enabled(log_TRACE));
    assert(raise(SIGUSR1) == 0);
    assert(log_enabled(log_TRACE));
    assert(raise(SIGUSR1) == 0);
    assert(log_enabled(log_TRACE));

    assert(raise(SIGUSR2) == 0);
    assert(log_enabled(log_DEBUG));
    assert(!log_enabled(log_TRACE));
    assert(raise(SIGUSR2) == 0);
    assert(!log_enabled(log_DEBUG));
    assert(log_enabled(log_INFO));
    assert(raise(SIGUSR2) == 0);
    assert(!log_enabled(log_INFO));
    assert(!log_enabled(log_ERROR));
    assert(log_enabled(log_WARNING));
    assert(log_enabled(log_OUTPUT));
    assert(log_enabled(log_SUCCESS));
    assert(log_enabled(log_FATAL));
    assert(raise(SIGUSR2) == 0);
    assert(log_enabled(log_SUCCESS));
    assert(log_enabled(log_FATAL));
    assert(log_enabled(log_WARNING));

    log_inc_level(0);
    assert(log_enabled(log_WARNING));
    return 0;
}
