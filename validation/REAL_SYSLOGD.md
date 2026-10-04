# Real local syslogd validation

Production v1 supports the existing local Unix-datagram Syslog backend only. This
validation closes the gap between mock/socket-level regression tests and an
actual syslog daemon.

## Environment

The dedicated `real-syslogd` workflow runs on Ubuntu 24.04 and installs the
distribution `rsyslog` package. The test launches `rsyslogd -n` directly
with a private configuration and private Unix socket; it does not depend on
systemd or the host `/dev/log`.

The daemon configuration:

- disables the system socket;
- loads a dedicated `imuxsock` input on a temporary pathname;
- disables per-input rate limiting;
- writes the raw received message to a temporary omfile destination;
- flushes file output at transaction end.

## Production-linked scenarios

The integration executable links the normal production `logger` target in both
shared and static builds. It does not link fault-injection or white-box support.

It validates:

1. REQUIRED startup rejects a missing endpoint;
2. REQUIRED startup with a live daemon delivers a real datagram and reports
   connected/sent metrics;
3. daemon shutdown breaks the existing association and the failed record is not
   replayed;
4. the reconnect cooldown prevents an immediate reconnect storm;
5. after a real daemon restart and cooldown expiry, a future record reconnects
   and is delivered;
6. DEFERRED startup accepts an unavailable endpoint, exposes the connect failure,
   and later recovers on a future record;
7. stopping a real daemon with SIGSTOP fills the real Unix-datagram receive
   queue until the nonblocking logger observes EAGAIN/ENOBUFS/ENOMEM
   backpressure;
8. metrics and diagnostics expose output failure and reconnect facts;
9. sticky output failure remains visible through flush/destroy even after a
   later successful reconnect.

## Boundary

This validates the supported local Unix-domain datagram contract against
rsyslogd. It does not add or claim:

- network Syslog over UDP/TCP/TLS;
- durable remote acknowledgement;
- failed-record replay;
- disk-backed spool ownership by c-logger;
- a hard real-time latency guarantee.

The existing syscall/fault regressions remain necessary for deterministic rare
errno coverage. This real-daemon workflow is complementary evidence rather than
a replacement for those tests.
