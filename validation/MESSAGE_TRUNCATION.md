# Visible bounded message truncation

Baseline: `e508bc071c833fa56769521b09d738d46887cd44`.

Ordinary void Logger calls retain at most 4095 message bytes. Previously an
oversized formatted message silently lost its tail. The bounded diagnostic now
ends with ` [truncated]`, including the Global pre-init bootstrap path. A final
ordinary output line exhausted by synchronous metadata also carries that marker
before LF. Smaller private formatter buffers use `~` when the full marker does
not fit. This is byte-oriented NUL-terminated text, not a UTF-8 or binary record
contract. No public ABI, per-record heap allocation, or new counter is added.

The acknowledged `logger_log_sync_status()` path continues to reject oversized
message or metadata with `-EOVERFLOW` before output. Existing emitted/failed and
completion counters still count actual records, verified after a quiescent flush.
Formatting failures continue to emit nothing. Untruncated bytes are unchanged.

## Verification

Xmake 3.1.1 / GCC 14.2 / Linux x86_64, final code based on the above commit:

- Release production static: 27/27 focused tests passed.
- Release production shared: 27/27 focused tests passed.
- Debug ASan + UBSan production static: 27/27 focused tests passed.
- Each matrix includes seven truncation scenarios, private formatter capacities
  0..5120, and production Console/stderr SIGPIPE regressions.
- Truncation scenarios cover explicit sync/async, Global, bootstrap, acknowledged
  status, and sync/async metadata. Message lengths are 0, 4, 4095, 4096, and 8191.
  Long module and separately long source labels exercise final-line overflow;
  tests require LF, exact line length, visible tail, no extra records, and metrics.
- The separate `tests/regression/run_audit_format.sh` script passed under
  ASan/UBSan: historical Audit checks use only the uninstalled support archive;
  checked formatter capacity/parity/encoding/nanosecond checks link production.
  The checked formatter harness is not an Xmake-registered test.
- Independent review reran static/shared seven scenarios, private small-buffer
  boundaries and checked formatter; production artifact checks and shared 47
  symbol ABI manifest comparison passed.

ASan used `detect_leaks=0` because the local leak scanner is restricted. This
focused matrix is not a full-suite or successful LSan claim. Complete CI is
required before merging.

Example focused invocation after configuring the selected static/shared mode:

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake test 'message_truncation_production_test/*' 'host_format_regression/*' \
  'console_sigpipe_production_test/*' 'stderr_sigpipe_production_test/*'
```

Before the fix, the same production fixture fails oversized ordinary and
bootstrap tail checks; acknowledged status already rejects correctly. A second
baseline probe with long synchronous metadata produced a 5119-byte line without
a visible marker, which motivated protecting the final-line boundary too.

The API and known-issues text also now records the Console thread-local SIGPIPE
behavior already implemented by baseline PR #155.

## Integration with flush release-proof fix

After PR #156 merged as `ae2671e2f3f39a705672b90cf8683c34a814d324`, the
final PR tree incorporates that main commit. The only textual conflict was
adjacent Xmake test registrations; both complete targets are retained. Runtime
changes combine without alteration. Fresh combined focused matrices passed:
Release static 39/39, Release shared 33/33, and ASan/UBSan static 39/39
(`detect_leaks=0`). The shared matrix includes six static white-box flush tests,
not interception inside the production DSO. Full CI must run on the updated PR
head rather than reusing the earlier standalone head's green checks.
