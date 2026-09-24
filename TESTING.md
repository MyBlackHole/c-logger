# Round 5 fork and production isolation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target check -j4
ctest --test-dir build -L fork-isolation --repeat until-fail:20 --output-on-failure -j4

# library-only (no tests or test fault archive)
cmake -S . -B build-lib -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-lib -j4
python3 scripts/check_production_artifact.py build-lib/liblogger.a
```

The existing fork_test is intentionally changed to **fork+exec**, preserving
parent/child message isolation and adding a CLOEXEC assertion. Exec-less child
reinit is now rejected explicitly by the new process_fork tests, not skipped.
The test-only wrappers assert rejection before inherited locks/once, allocation,
formatting, thread creation, descriptor close or write. All original fault/crash scenarios remain registered. Environment-driven hook
cases explicitly link logger_test_support, including the new positive controls.
Other regressions default to the actual production logger; their syscall/schedule
wrappers are test-executable-only. Both libraries share implementation sources
and public API but differ deliberately in the private hook compile definition.

See validation/ROUND5_RESULTS.md for measured results and retained failures.
No sanitizer suppression is added to make an external-library report disappear.

## 本轮验证范围

新增 `queue-flush` label，运行 `ctest --test-dir build -L queue-flush --output-on-failure`。
`check` 从空 build 目录会先构建测试程序；labels 追加而不覆盖；各测试独立工作目录。
专项 sanitizer 结果见 `validation/RESULTS.md`。该段记录 Round 1 的历史状态；当前剩余阻断项以 docs/KNOWN_ISSUES.md 为准。

# Test strategy

Tests are classified with CTest labels so local development and CI can run the
right reliability level without maintaining separate test lists.

| Label | Purpose |
|---|---|
| `fast` | small developer feedback set |
| `unit` | API/helpers/format/config contracts |
| `integration` | file, rotation, audit, syslog end-to-end behavior |
| `concurrency` | MPSC/lifecycle/fork concurrency |
| `fork` | explicit and defensive fork contracts |
| `fault` | deterministic syscall failure injection |
| `crash` | real child-process death at durability boundaries |
| `reliability` | fault + crash verification |
| `security` | audit integrity, recovery, single writer, redaction |
| `crypto` | SHA-256 chain, known-answer tests, and retired-algorithm rejection |
| `async-cancel` | deliberate `PTHREAD_CANCEL_ASYNCHRONOUS` stress cases; mandatory in native runs, excluded under sanitizer runtimes |

## Local commands

After configuring and building:

```sh
cmake --build build --target check-fast
cmake --build build --target check
cmake --build build --target check-production
```

Or use:

```sh
scripts/check.sh fast
scripts/check.sh reliability
scripts/check.sh security
scripts/check.sh production
```

`check-production` currently means the complete configured test suite. Sanitizer
builds remain separate build directories because compiler instrumentation is a
build property, not a CTest runtime profile.

## Release gate

A production release should pass the complete normal suite, both builtin
digest known-answer/integrity tests, and ASan/UBSan/TSan in a CI runner whose
virtual-address environment supports those runtimes. The sanitizer profiles run
the complete suite except tests labeled `async-cancel`: those cases deliberately
enable `PTHREAD_CANCEL_ASYNCHRONOUS`, while sanitizer runtimes instrument the
pthread/cancellation path itself and are not a reliable oracle for that delivery
contract. The same cases remain mandatory in the unsanitized complete native
suite; they are not skipped from the release gate. No external crypto package is
used to build or run the current test suite.

## Round 2 Audit state regressions

`ctest --test-dir build -L audit-state --output-on-failure` runs the 25 new cases.
They use private syscall/scheduling wrappers linked only into test executables.
`--repeat until-fail:30 -j4` runs each case 30 times; test directories are independent.
Legacy Audit contention/crash tests now use fork+exec instead of calling complex
Audit initialization in an inherited child. Test assertions are not relaxed.

Source and exact validation scope are recorded in `validation/ROUND2_RESULTS.md`.
Full normal suites passing do not close the still-known crypto/recovery blockers.

## Crypto regression profile (current)

`crypto-failclosed` retains the two digest contracts, fixed known-answer and
binary fixtures, concurrent hashing, START/event/STOP failure propagation,
verification/recovery failures and empty/no-replay preflight. Failure wrappers
exist only in test executables; no production runtime error injector is added.
External-backend stage/policy tests are removed with the implementation they
exercised, not suppressed or relabeled as passing. Earlier reports remain in
validation for provenance.

```sh
ctest --test-dir build -L '^crypto$' --output-on-failure
ctest --test-dir build -L '^crypto-failclosed$' --repeat until-fail:20 --output-on-failure
ctest --test-dir build -L '^crypto-builtin-only$' --output-on-failure
python3 scripts/crypto_cross_version.py reference-build candidate-build
```

The last script is optional migration tooling: build `crypto_chain_tool` in
both directories. It compares two provided versions/builds without selecting a
crypto dependency. Normal tests use checked-in legacy log/checkpoint fixtures;
they need no reference binary or external crypto provider. Digest fixtures are
frozen, independently generated data, never regenerated from the code under
test. See tests/fixtures/CRYPTO_PROVENANCE.md.

ASan/UBSan: `-DLOGGER_SANITIZE=address-undefined`; TSan: `thread`, in separate
builds. Keep leak detection enabled. Run `ctest -LE '^async-cancel$'` under
sanitizers and run the complete uninstrumented suite separately, including every
`async-cancel` case. Any additional sanitizer exclusion is a release-blocking
change that must be documented rather than hidden behind a passing subset.

## Round 4 audit-parser label

```sh
ctest --test-dir build -L audit-parser --output-on-failure
ctest --test-dir build -L audit-parser --repeat until-fail:20 --output-on-failure
```

160 deterministic cases cover shared grammar, rehashed-but-malformed input,
bounded reads, stream errors, full/partial EOF, checkpoint parsing, file-set
continuity, ambiguity, evidence persistence failures and fork+exec rotation
crashes. Tests use isolated temp directories and link-time wrappers; no new
production runtime fault switch was added. Fault hooks are now isolated in logger_test_support; the Round 4 label does
not imply whole-project production readiness. Complete sanitizer suites must
still be run. Old fork timeout evidence remains in the historical reports.

## Host-owned / shared 集成

`ctest -L host-owned` / `scripts/check.sh host-owned`。
默认不生成23项legacy fork helper测试；显式开启兼容选项时生成并运行，不将其标记skip。
shared配置的白盒 --wrap 测试链接同源static regression库，非带hooks库；普通集成仍链接共享库。
`host_owned_example`、`liblogger_dlclose` 是实际shared调用测试，SDK自身无Logger依赖。
默认生产artifact可由 `scripts/check_production_artifact.py liblogger.so` 检查；兼容产物加 `--legacy-fork`。

## File-backend regression profile

`ctest --test-dir build -L file-backend --output-on-failure` selects the new
owner/reopen/no-clobber/path/rotation/crash regressions. White-box wrappers link
the same source as a static test support library in shared configurations;
host-owned and dlclose integration still exercise the real shared production
library. Black-box probes only use pre-existing public APIs and are suitable
for reverse-testing the input archive. Each black-box run uses a fresh private
directory, including repeats. Never remove live production lock files.

## Global lifecycle 专项

`ctest --test-dir build -L '^global-lifecycle$' --output-on-failure` 覆盖准入、
代次、取消、重入、错误回滚及并发生命周期。`global-cancel` 是其中的取消子集。
白盒同步点只在测试链接 wrapper 中；共享构建另含 `global_shared_*` 的实际 DSO
压力测试，不把 --wrap 静态白盒误称为 DSO 内部拦截。

## Explicit/Console scope profile

`ctest --test-dir build -L explicit-scope --output-on-failure` runs deterministic
cancellation, constructor ownership, callback reentry and real host FILE callback
tests. Shared builds use the same-source static regression library for --wrap
cases and the real production DSO for the unwrapped host callback integration.

## Release engineering candidate

Native install-enabled builds add `packaging_install_relocate`. `check-package` builds the production target, stages and relocates a real installation, then builds independent C/C++11 and pkg-config consumers plus a PIC SDK plugin. Sanitizer builds default to installation disabled; they do not contain this distribution test. Public DSO tests must not import private symbols: crypto white-box tests and the /dev/null private-barrier benchmark use same-source static production objects. The real shared global-stress variant checks externally visible fd cleanup/status; static regressions retain the private object-count assertion. See docs/RELEASE_ENGINEERING.md.
