// SPDX-License-Identifier: GPL-3.0-only
//
// Watchdog-only stub: sd_notify(READY=1) then a signalfd+timerfd
// poll loop that pings WATCHDOG=1 every 10 s against the systemd
// unit's WatchdogSec=30 ceiling (3× safety margin). SIGTERM or
// SIGINT triggers sd_notify(STOPPING=1) + clean exit. No JSON-RPC,
// no TLS, no worker pool — the real daemon is future work.
//
// The file is deliberately not a "return 0;" one-liner because the
// watchdog fault-injection DEP-8 test needs a live Type=notify
// process to exercise the liveness path end to end.
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>

namespace {
constexpr int kWatchdogPeriodSeconds = 10;
}

int main(int /*argc*/, char* /*argv*/[]) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        sd_notifyf(0, "STATUS=signalfd() failed: errno=%d\nSTOPPING=1", errno);
        return 1;
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) {
        sd_notifyf(0, "STATUS=timerfd_create() failed: errno=%d\nSTOPPING=1", errno);
        return 1;
    }

    itimerspec ts{};
    ts.it_value.tv_sec = kWatchdogPeriodSeconds;
    ts.it_interval.tv_sec = kWatchdogPeriodSeconds;
    if (timerfd_settime(tfd, 0, &ts, nullptr) < 0) {
        sd_notifyf(0, "STATUS=timerfd_settime() failed: errno=%d\nSTOPPING=1", errno);
        return 1;
    }

    sd_notify(0, "READY=1\nSTATUS=watchdog-only stub");

    pollfd pfds[2] = {
        {sfd, POLLIN, 0},
        {tfd, POLLIN, 0},
    };

    for (;;) {
        int rc = poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            sd_notifyf(0, "STATUS=poll() failed: errno=%d\nSTOPPING=1", errno);
            return 1;
        }
        if (pfds[0].revents & POLLIN) {
            sd_notify(0, "STOPPING=1\nSTATUS=Received termination signal");
            return 0;
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t expirations = 0;
            ssize_t n = read(tfd, &expirations, sizeof(expirations));
            (void)n;
            sd_notify(0, "WATCHDOG=1");
        }
    }
}
