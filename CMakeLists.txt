cmake_minimum_required(VERSION 3.16)
project(prod_c_logger VERSION 0.9.0 LANGUAGES C)
# First packaged development snapshot, not a certified Production v1.
set(LOGGER_ABI_VERSION 0)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This release is Linux/ELF only; other platforms are not validated.")
endif()
include(GNUInstallDirs)
find_package(Threads REQUIRED)

option(BUILD_SHARED_LIBS "Build the production Logger as a shared library" OFF)
option(LOGGER_ENABLE_LEGACY_FORK_HELPER
       "Compatibility only: build logger_fork_reinit (owns process creation)" OFF)

# The digest implementation is always self-contained. This is a migration
# diagnostic, not a backend selector. Never silently reinterpret an old request
# for an external crypto policy as builtin.
if(DEFINED LOGGER_CRYPTO_BACKEND)
  if(NOT LOGGER_CRYPTO_BACKEND STREQUAL "builtin")
    message(FATAL_ERROR
      "LOGGER_CRYPTO_BACKEND has been removed. Only builtin SHA-256 is provided; remove the old option and reconfigure.")
  endif()
  message(DEPRECATION "LOGGER_CRYPTO_BACKEND=builtin is obsolete; remove this option.")
  unset(LOGGER_CRYPTO_BACKEND CACHE)
  unset(LOGGER_CRYPTO_BACKEND)
endif()

set(LOGGER_SANITIZE "" CACHE STRING "Sanitizer: address, undefined, address-undefined, or thread")
if(LOGGER_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(LOGGER_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(LOGGER_SANITIZE STREQUAL "address-undefined")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(LOGGER_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT LOGGER_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown LOGGER_SANITIZE=${LOGGER_SANITIZE}")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Production never links the environment-driven test fault injector.
set(LOGGER_SOURCES
  src/logger.c
  src/logger_global.c
  src/logger_process.c
  src/logger_scope.c
  src/logger_redact.c
  src/logger_format.c
  src/logger_queue.c
  src/logger_file.c
  src/logger_file_rename.c
  src/logger_syslog.c
  src/logger_worker.c
  src/audit.c
  src/audit_record.c
  src/audit_integrity.c
  src/audit_recovery.c
  src/audit_verify.c
  src/console.c
)
if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
  list(APPEND LOGGER_SOURCES src/logger_fork.c)
endif()
function(configure_logger target)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/logger>")
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
  get_target_property(_kind ${target} TYPE)
  if(_kind STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(${target} PUBLIC LOGGER_STATIC_DEFINE=1)
  endif()
  if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
    target_compile_definitions(${target} PUBLIC LOGGER_ENABLE_LEGACY_FORK_HELPER=1)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(${target} PUBLIC Threads::Threads)
endfunction()
add_library(logger ${LOGGER_SOURCES})
configure_logger(logger)
add_library(Logger::logger ALIAS logger)
include(cmake/LoggerPackaging.cmake)
target_compile_definitions(logger PRIVATE LOGGER_ENABLE_FAULT_INJECTION=0)
set_property(TARGET logger PROPERTY INTERFACE_LOGGER_VARIANT production)
set_property(TARGET logger APPEND PROPERTY COMPATIBLE_INTERFACE_STRING LOGGER_VARIANT)

include(CTest)
if(BUILD_TESTING)
if(BUILD_SHARED_LIBS)
  add_library(logger_regression_support STATIC EXCLUDE_FROM_ALL ${LOGGER_SOURCES})
  configure_logger(logger_regression_support)
  # White-box regressions use --wrap against libc entry points. Release/FORTIFY
  # may rewrite calls such as vsnprintf/vfprintf/dprintf to __*_chk symbols,
  # bypassing those synchronization hooks. Keep the production shared library
  # fortified; only the private same-source regression archive uses stable
  # unfortified libc symbol names.
  target_compile_options(logger_regression_support PRIVATE
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
  target_compile_definitions(logger_regression_support PRIVATE LOGGER_ENABLE_FAULT_INJECTION=0)
  set_property(TARGET logger_regression_support PROPERTY INTERFACE_LOGGER_VARIANT production)
  set_property(TARGET logger_regression_support APPEND PROPERTY COMPATIBLE_INTERFACE_STRING LOGGER_VARIANT)
  set(LOGGER_REGRESSION_LIBRARY logger_regression_support)
else()
  set(LOGGER_REGRESSION_LIBRARY logger)
endif()
# Never link both targets into an application. The visibly test-only archive is
# not a production option; it exists only when tests are configured.
add_library(logger_test_support STATIC EXCLUDE_FROM_ALL ${LOGGER_SOURCES} src/logger_fault.c)
configure_logger(logger_test_support)
target_compile_definitions(logger_test_support PRIVATE LOGGER_ENABLE_FAULT_INJECTION=1)
set_property(TARGET logger_test_support PROPERTY INTERFACE_LOGGER_VARIANT test-only)
set_property(TARGET logger_test_support APPEND PROPERTY COMPATIBLE_INTERFACE_STRING LOGGER_VARIANT)
add_executable(test_logger tests/test_logger.c)
target_link_libraries(test_logger PRIVATE logger)
enable_testing()
add_test(NAME logger_test COMMAND test_logger)

add_executable(bench_logger tests/bench_logger.c)
target_link_libraries(bench_logger PRIVATE logger)

add_executable(rotation_test tests/test_rotation.c)
target_link_libraries(rotation_test PRIVATE logger)
add_test(NAME rotation_test COMMAND rotation_test)

add_executable(audit_test tests/test_audit.c)
target_link_libraries(audit_test PRIVATE logger)
add_test(NAME audit_test COMMAND audit_test)

add_executable(audit_failure_test tests/test_audit_failure.c)
target_link_libraries(audit_failure_test PRIVATE logger)
add_test(NAME audit_failure_test COMMAND audit_failure_test)

add_executable(audit_transaction_test tests/test_audit_transaction.c)
target_link_libraries(audit_transaction_test PRIVATE logger)
add_test(NAME audit_transaction_test COMMAND audit_transaction_test)

add_executable(console_test tests/test_console.c)
target_link_libraries(console_test PRIVATE logger)
add_test(NAME console_test COMMAND console_test)

add_executable(format_test tests/test_format.c)
target_link_libraries(format_test PRIVATE logger)
add_test(NAME format_test COMMAND format_test)

add_executable(context_test tests/test_context.c)
target_link_libraries(context_test PRIVATE logger)
add_test(NAME context_test COMMAND context_test)

add_executable(permissions_test tests/test_permissions.c)
target_link_libraries(permissions_test PRIVATE logger)
add_test(NAME permissions_test COMMAND permissions_test)

add_executable(audit_instance_test tests/test_audit_instance.c)
target_link_libraries(audit_instance_test PRIVATE logger)
add_test(NAME audit_instance_test COMMAND audit_instance_test)

add_executable(audit_integrity_test tests/test_audit_integrity.c)
target_link_libraries(audit_integrity_test PRIVATE logger)
add_test(NAME audit_integrity_test COMMAND audit_integrity_test)

add_executable(audit_restart_test tests/test_audit_restart.c)
target_link_libraries(audit_restart_test PRIVATE logger)
add_test(NAME audit_restart_test COMMAND audit_restart_test)

add_executable(audit_recovery_test tests/test_audit_recovery.c)
target_link_libraries(audit_recovery_test PRIVATE logger)
add_test(NAME audit_recovery_test COMMAND audit_recovery_test)

add_executable(audit_rotation_recovery_test tests/test_audit_rotation_recovery.c)
target_link_libraries(audit_rotation_recovery_test PRIVATE logger)
add_test(NAME audit_rotation_recovery_test COMMAND audit_rotation_recovery_test)

add_executable(fork_test tests/test_fork.c)
target_link_libraries(fork_test PRIVATE logger)
add_test(NAME fork_test COMMAND fork_test)

add_executable(audit_fork_test tests/test_audit_fork.c)
target_link_libraries(audit_fork_test PRIVATE logger)
add_test(NAME audit_fork_test COMMAND audit_fork_test)

add_executable(fork_guard_test tests/test_fork_guard.c)
target_link_libraries(fork_guard_test PRIVATE logger)
add_test(NAME fork_guard_test COMMAND fork_guard_test)
set_tests_properties(fork_guard_test PROPERTIES TIMEOUT 5)

add_executable(lifecycle_test tests/test_lifecycle.c)
target_link_libraries(lifecycle_test PRIVATE logger)
add_test(NAME lifecycle_test COMMAND lifecycle_test)
set_tests_properties(lifecycle_test PROPERTIES TIMEOUT 10)

add_executable(fault_test tests/test_faults.c)
target_link_libraries(fault_test PRIVATE logger_test_support)
add_test(NAME fault_file_write COMMAND fault_test logger)
set_tests_properties(fault_file_write PROPERTIES ENVIRONMENT "LOGGER_FAULT_POINT=file_write;LOGGER_FAULT_ERRNO=5")
add_test(NAME fault_file_fsync COMMAND fault_test logger)
set_tests_properties(fault_file_fsync PROPERTIES ENVIRONMENT "LOGGER_FAULT_POINT=file_fsync;LOGGER_FAULT_ERRNO=5")
add_test(NAME fault_state_write COMMAND fault_test audit)
set_tests_properties(fault_state_write PROPERTIES ENVIRONMENT "LOGGER_FAULT_POINT=state_write;LOGGER_FAULT_ERRNO=5")
add_test(NAME fault_state_fsync COMMAND fault_test audit)
set_tests_properties(fault_state_fsync PROPERTIES ENVIRONMENT "LOGGER_FAULT_POINT=state_fsync;LOGGER_FAULT_ERRNO=5")
add_test(NAME fault_state_rename COMMAND fault_test audit)
set_tests_properties(fault_state_rename PROPERTIES ENVIRONMENT "LOGGER_FAULT_POINT=state_rename;LOGGER_FAULT_ERRNO=5")

add_executable(crash_recovery_test tests/test_crash_recovery.c)
target_link_libraries(crash_recovery_test PRIVATE logger_test_support)
foreach(point after_audit_fsync before_state_rename after_state_rename after_checkpoint_commit)
  add_test(NAME crash_${point} COMMAND crash_recovery_test ${point})
  set_tests_properties(crash_${point} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(concurrency_stress_test tests/test_concurrency_stress.c)
target_link_libraries(concurrency_stress_test PRIVATE logger)
add_test(NAME concurrency_stress_test COMMAND concurrency_stress_test)
set_tests_properties(concurrency_stress_test PROPERTIES TIMEOUT 20)


add_executable(api_contract_test tests/test_api_contract.c)
target_link_libraries(api_contract_test PRIVATE logger)
add_test(NAME api_contract_test COMMAND api_contract_test)

add_executable(config_abi_test tests/test_config_abi.c)
target_link_libraries(config_abi_test PRIVATE logger)
add_test(NAME config_abi_test COMMAND config_abi_test)

add_executable(audit_single_writer_test tests/test_audit_single_writer.c)
target_link_libraries(audit_single_writer_test PRIVATE logger)
add_test(NAME audit_single_writer_test COMMAND audit_single_writer_test)

add_executable(syslog_multi_test tests/test_syslog_multi.c)
target_link_libraries(syslog_multi_test PRIVATE logger_test_support)
add_test(NAME syslog_multi_test COMMAND syslog_multi_test)
set_tests_properties(syslog_multi_test PROPERTIES TIMEOUT 5)

add_executable(crypto_vectors_test tests/test_crypto_vectors.c)
target_link_libraries(crypto_vectors_test PRIVATE ${LOGGER_REGRESSION_LIBRARY})
add_test(NAME crypto_vectors_test COMMAND crypto_vectors_test)

add_executable(redaction_test tests/test_redaction.c)
target_link_libraries(redaction_test PRIVATE logger)
add_test(NAME redaction_test COMMAND redaction_test)

# LOGGER_TEST_LABELS_BEGIN
set_property(TEST
  logger_test format_test context_test permissions_test console_test
  api_contract_test config_abi_test crypto_vectors_test redaction_test
  APPEND PROPERTY LABELS "unit;fast"
)
set_property(TEST
  rotation_test audit_test audit_failure_test audit_transaction_test
  audit_instance_test audit_integrity_test audit_restart_test
  audit_recovery_test audit_rotation_recovery_test audit_single_writer_test
  syslog_multi_test
  APPEND PROPERTY LABELS "integration"
)
set_property(TEST
  lifecycle_test concurrency_stress_test
  APPEND PROPERTY LABELS "concurrency"
)
set_property(TEST
  fork_test audit_fork_test fork_guard_test
  APPEND PROPERTY LABELS "fork;concurrency"
)
set_property(TEST
  fault_file_write fault_file_fsync fault_state_write fault_state_fsync
  fault_state_rename
  APPEND PROPERTY LABELS "fault;reliability"
)
set_property(TEST
  crash_after_audit_fsync crash_before_state_rename crash_after_state_rename
  crash_after_checkpoint_commit
  APPEND PROPERTY LABELS "crash;reliability"
)
set_property(TEST
  audit_integrity_test audit_recovery_test audit_rotation_recovery_test
  audit_single_writer_test redaction_test
  APPEND PROPERTY LABELS "security"
)
set_property(TEST
  audit_integrity_test crypto_vectors_test
  APPEND PROPERTY LABELS "crypto"
)

add_custom_target(check-fast
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L fast
  DEPENDS logger
  USES_TERMINAL
)
add_custom_target(check
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS logger
  USES_TERMINAL
)
add_custom_target(check-production
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS logger
  USES_TERMINAL
)
# LOGGER_TEST_LABELS_END

add_executable(bench_matrix tests/bench_matrix.c)
# This benchmark uses the private output barrier to avoid fsync(/dev/null).
# Keep it on the same-source static production code, not the public DSO ABI.
target_link_libraries(bench_matrix PRIVATE ${LOGGER_REGRESSION_LIBRARY})

add_custom_target(benchmark
  COMMAND ${CMAKE_COMMAND} -E env BUILD_DIR=${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/scripts/benchmark.sh
  DEPENDS bench_matrix
  USES_TERMINAL
)

add_executable(reinit_test tests/test_reinit.c)
target_link_libraries(reinit_test PRIVATE logger)
add_test(NAME reinit_test COMMAND reinit_test)
set_tests_properties(reinit_test PROPERTIES LABELS "concurrency;unit" TIMEOUT 10)

add_executable(multi_instance_example examples/multi_instance.c)
target_link_libraries(multi_instance_example PRIVATE logger)

add_executable(multi_instance_test tests/test_multi_instance.c)
target_link_libraries(multi_instance_test PRIVATE logger)
add_test(NAME multi_instance_test COMMAND multi_instance_test)
set_tests_properties(multi_instance_test PROPERTIES LABELS "unit;integration")

# Deterministic correctness regressions. Scheduling and syscall wrappers exist
# only in these executables, never in the production logging hot path.
function(logger_regression target source)
  add_executable(${target} tests/regression/${source})
  if(ARGC GREATER 2)
    target_link_libraries(${target} PRIVATE ${ARGV2})
  else()
    target_link_libraries(${target} PRIVATE ${LOGGER_REGRESSION_LIBRARY})
  endif()
  target_include_directories(${target} PRIVATE src)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()
logger_regression(queue_notify_regression test_queue_notify.c)
target_link_options(queue_notify_regression PRIVATE -Wl,--wrap=pthread_cond_wait)
logger_regression(flush_pending_regression test_flush_pending.c)
target_link_options(flush_pending_regression PRIVATE -Wl,--wrap=logger_format_line -Wl,--wrap=pthread_cond_wait -Wl,--wrap=fsync)
logger_regression(flush_watermark_regression test_flush_watermark.c)
target_link_options(flush_watermark_regression PRIVATE -Wl,--wrap=logger_format_line -Wl,--wrap=pthread_cond_wait)
logger_regression(io_accounting_regression test_io_accounting.c)
target_link_options(io_accounting_regression PRIVATE -Wl,--wrap=logger_format_line -Wl,--wrap=write -Wl,--wrap=writev -Wl,--wrap=fsync)
logger_regression(stderr_sigpipe_regression test_stderr_sigpipe.c)
logger_regression(queue_mpsc_regression test_queue_mpsc.c)
logger_regression(resource_cleanup_regression test_resource_cleanup.c)
target_link_options(resource_cleanup_regression PRIVATE -Wl,--wrap=free)
logger_regression(global_flush_regression test_global_flush.c)
foreach(t queue_notify_regression flush_pending_regression flush_watermark_regression queue_mpsc_regression global_flush_regression)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES LABELS "regression;concurrency;queue-flush" TIMEOUT 15)
endforeach()
add_test(NAME resource_cleanup_regression COMMAND resource_cleanup_regression)
set_tests_properties(resource_cleanup_regression PROPERTIES
  LABELS "regression;resource-lifecycle;reliability" TIMEOUT 15)
foreach(s drop fallback fallback-error async-error short-ok short-error file-fsync dir-fsync rotation-fsync sync)
  add_test(NAME io_${s}_regression COMMAND io_accounting_regression ${s})
  set_tests_properties(io_${s}_regression PROPERTIES LABELS "regression;reliability;queue-flush" TIMEOUT 15)
endforeach()
foreach(s sync async preblocked)
  add_test(NAME stderr_sigpipe_${s} COMMAND stderr_sigpipe_regression ${s})
  set_tests_properties(stderr_sigpipe_${s} PROPERTIES LABELS "regression;reliability;host-owned" TIMEOUT 15)
endforeach()
target_include_directories(bench_matrix PRIVATE src)
target_compile_options(bench_matrix PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME benchmark_accounting_smoke COMMAND bench_matrix 4 1000 64)
set_tests_properties(benchmark_accounting_smoke PROPERTIES LABELS "regression;queue-flush;benchmark-smoke" TIMEOUT 15)

# Audit lifecycle/commit regressions use link-time wrappers in test executables.
logger_regression(audit_commit_regression test_audit_commit.c)
target_link_options(audit_commit_regression PRIVATE
  -Wl,--wrap=audit_checkpoint_persist -Wl,--wrap=logger_log_sync_status
  -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=logger_file_offset)
foreach(s checkpoint-once offset-error checkpoint-persistent io-write io-partial io-fsync preflight stop-error start-error)
  add_test(NAME audit_${s}_regression COMMAND audit_commit_regression ${s})
  set_tests_properties(audit_${s}_regression PROPERTIES LABELS "regression;reliability;audit-state" TIMEOUT 20)
endforeach()
logger_regression(audit_lifecycle_regression test_audit_lifecycle.c)
target_link_options(audit_lifecycle_regression PRIVATE
  -Wl,--wrap=logger_destroy_status -Wl,--wrap=logger_log_sync_status -Wl,--wrap=pthread_mutex_lock)
foreach(s init-gate stop-gate teardown-race generation fork-guard cancel)
  add_test(NAME audit_${s}_regression COMMAND audit_lifecycle_regression ${s})
  set_tests_properties(audit_${s}_regression PROPERTIES LABELS "regression;concurrency;audit-state" TIMEOUT 20)
endforeach()
logger_regression(checkpoint_io_regression test_checkpoint_io.c)
target_link_options(checkpoint_io_regression PRIVATE
  -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=rename)
foreach(s ok write zero short-eintr fsync dir-fsync close rename)
  add_test(NAME checkpoint_${s}_regression COMMAND checkpoint_io_regression ${s})
  set_tests_properties(checkpoint_${s}_regression PROPERTIES LABELS "regression;reliability;audit-state" TIMEOUT 20)
endforeach()
logger_regression(audit_concurrency_regression test_audit_concurrency.c)
foreach(s lifetimes transactions)
  add_test(NAME audit_${s}_regression COMMAND audit_concurrency_regression ${s})
  set_tests_properties(audit_${s}_regression PROPERTIES LABELS "regression;concurrency;audit-state" TIMEOUT 30)
endforeach()

# Round 3: crypto errors must be distinct from digest bytes. Wrappers are test-only.
logger_regression(crypto_chain_tool crypto_chain_tool.c)
logger_regression(crypto_contract_regression test_crypto_contract.c)
logger_regression(crypto_failure_regression test_crypto_failure.c)
target_link_options(crypto_failure_regression PRIVATE -Wl,--wrap=audit_digest_provider)
foreach(algorithm sha256)
  foreach(s vectors boundaries invalid threads)
    add_test(NAME crypto_${algorithm}_${s} COMMAND crypto_contract_regression ${s} ${algorithm})
    set_tests_properties(crypto_${algorithm}_${s} PROPERTIES LABELS "regression;crypto;crypto-failclosed" TIMEOUT 30)
  endforeach()
  foreach(s init-probe init-start write begin end stop verify verify-empty recover-probe recover-forward recover-init)
    add_test(NAME crypto_${algorithm}_${s} COMMAND crypto_failure_regression ${s} ${algorithm})
    set_tests_properties(crypto_${algorithm}_${s} PROPERTIES LABELS "regression;crypto;reliability;crypto-failclosed" TIMEOUT 20)
  endforeach()
endforeach()
add_test(NAME crypto_none_explicit COMMAND crypto_failure_regression none sha256)
set_tests_properties(crypto_none_explicit PROPERTIES LABELS "regression;crypto;crypto-failclosed" TIMEOUT 20)
# Round 4: one strict codec and bounded reader across writer/verifier/recovery.
logger_regression(audit_record_regression test_audit_record.c)
logger_regression(audit_recovery_strict_regression test_audit_recovery_strict.c)
logger_regression(audit_reader_regression test_audit_reader.c)
logger_regression(audit_tail_io_regression test_audit_tail_io.c)
target_link_options(audit_tail_io_regression PRIVATE -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=ftruncate)
logger_regression(audit_rotation_crash_regression test_audit_rotation_crash.c logger_test_support)
target_link_options(audit_rotation_crash_regression PRIVATE -Wl,--wrap=logger_fault_crash_if_requested)
foreach(algorithm sha256)
  foreach(s roundtrip bounds fuzz suffix duplicate unknown reorder escape escape-nul escape-print raw-control unclosed
            seq-zero seq-overflow seq-leading seq-negative txn-plus txn-overflow error-overflow error-underflow negative-zero
            phase result attempt-result missing-result empty-event empty-operation instance bad-prefix bad-date trailing-space
            crlf missing-lf short-hash long-hash hash-nonhex hash-upper oversize oversize-eof nul)
    add_test(NAME record_${algorithm}_${s} COMMAND audit_record_regression ${s} ${algorithm})
    set_tests_properties(record_${algorithm}_${s} PROPERTIES LABELS "regression;security;audit-parser" TIMEOUT 30)
  endforeach()
  foreach(s forward partial complete-no-lf oversize-complete oversize-eof malformed nul checkpoint-offset checkpoint-ahead
            checkpoint-seq before-checkpoint-corrupt names archives time-backwards active-larger no-active empty-active
            archive-partial missing-middle duplicate branch active-not-last lost-state-genesis lost-state-retained
            retained-anchor symlink fifo)
    add_test(NAME recovery_${algorithm}_${s} COMMAND audit_recovery_strict_regression ${s} ${algorithm})
    set_tests_properties(recovery_${algorithm}_${s} PROPERTIES LABELS "regression;security;reliability;audit-parser" TIMEOUT 30)
  endforeach()
  add_test(NAME rotation_crash_${algorithm} COMMAND audit_rotation_crash_regression rotation ${algorithm})
  set_tests_properties(rotation_crash_${algorithm} PROPERTIES LABELS "regression;crash;reliability;audit-parser" TIMEOUT 30)
endforeach()
foreach(s reader ok suffix second-line second-blank negative-offset large-offset leading-seq unknown-alg short-hash long-hash
          short-crc unknown-version missing-lf nul bad-crc)
  add_test(NAME strict_reader_${s} COMMAND audit_reader_regression ${s})
  set_tests_properties(strict_reader_${s} PROPERTIES LABELS "regression;security;audit-parser" TIMEOUT 20)
endforeach()
foreach(s ok write zero short-eintr evidence-fsync directory-fsync truncate active-fsync)
  add_test(NAME tail_io_${s} COMMAND audit_tail_io_regression ${s})
  set_tests_properties(tail_io_${s} PROPERTIES LABELS "regression;reliability;audit-parser" TIMEOUT 20)
endforeach()

# Test orchestration only: no test may depend on files of a different CTest case.
# Each case still performs its own cleanup to remain repeatable in this directory.
# Round 5: inherited runtimes are rejected, exec creates a fresh runtime.
logger_regression(process_fork_regression test_process_fork.c)
target_link_options(process_fork_regression PRIVATE
  -Wl,--wrap=pthread_atfork -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=pthread_rwlock_rdlock -Wl,--wrap=pthread_rwlock_wrlock
  -Wl,--wrap=pthread_once -Wl,--wrap=calloc -Wl,--wrap=free
  -Wl,--wrap=pthread_create -Wl,--wrap=write -Wl,--wrap=close
  -Wl,--wrap=vsnprintf -Wl,--wrap=vfprintf -Wl,--wrap=logger_vlog_internal)
foreach(s global explicit console-only context-only audit-only verify-only emit-held progress-held
          console-held reader-held registration-window earlier-handler after-shutdown prepare bypass-handler prefork
          register-global register-explicit register-console register-context register-audit register-verify)
  add_test(NAME process_fork_${s} COMMAND process_fork_regression ${s})
  set_tests_properties(process_fork_${s} PROPERTIES LABELS "regression;fork;fork-isolation" TIMEOUT 12 SKIP_RETURN_CODE 77)
endforeach()
set_tests_properties(fork_test PROPERTIES TIMEOUT 15)
set_property(TEST fork_test APPEND PROPERTY LABELS "regression;fork-isolation")
target_compile_options(fork_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
logger_regression(production_isolation_regression test_fault_isolation.c)
logger_regression(test_hooks_regression test_fault_isolation.c logger_test_support)
target_compile_definitions(test_hooks_regression PRIVATE TEST_EXPECT_HOOKS=1)
foreach(t production_isolation_regression test_hooks_regression)
  target_link_options(${t} PRIVATE -Wl,--wrap=connect)
endforeach()
foreach(s file_write file_fsync file_rename file_ftruncate state_write state_fsync state_rename
          short-write syslog-path after_audit_fsync before_state_rename after_state_rename after_checkpoint_commit)
  add_test(NAME production_isolation_${s} COMMAND production_isolation_regression ${s})
  add_test(NAME test_hooks_${s} COMMAND test_hooks_regression ${s})
  set_tests_properties(production_isolation_${s} test_hooks_${s} PROPERTIES
    LABELS "regression;security;fork-isolation;production-isolation" TIMEOUT 15)
endforeach()

if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
# Controlled single-thread fork/continue. Runtime allocation happens only before
# or after the clean fork boundary, never by resetting inherited pthread objects.
logger_regression(fork_reinit_regression test_fork_reinit.c)
target_link_options(fork_reinit_regression PRIVATE
  -Wl,--wrap=fork -Wl,--wrap=opendir -Wl,--wrap=close
  -Wl,--wrap=calloc -Wl,--wrap=pthread_atfork)
foreach(s async sync drain no-init console-only after-shutdown context raw-guard
          thread-busy explicit-busy explicit-only audit-busy proc-failure fork-failure
          io-failure close-failure creation-failure registration-failure earlier-handler nested repeat explicit-recreate audit-conflict)
  add_test(NAME fork_reinit_${s} COMMAND fork_reinit_regression ${s})
  set_tests_properties(fork_reinit_${s} PROPERTIES LABELS "regression;fork;fork-reinit" TIMEOUT 20)
endforeach()

add_executable(fork_reinit_example examples/fork_reinit.c)
target_link_libraries(fork_reinit_example PRIVATE logger)
target_compile_options(fork_reinit_example PRIVATE -Wall -Wextra -Wpedantic -Werror)

endif() # legacy helper tests are opt-in, not silently skipped

# Host-owned integration example. This SDK intentionally has NO logger link.
add_library(example_sdk SHARED examples/host_owned/example_sdk.c)
target_include_directories(example_sdk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/examples/host_owned")
target_compile_options(example_sdk PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(host_owned_example examples/host_owned/host.c)
target_include_directories(host_owned_example PRIVATE examples/host_owned)
target_link_libraries(host_owned_example PRIVATE logger ${CMAKE_DL_LIBS})
target_compile_options(host_owned_example PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_dependencies(host_owned_example example_sdk)
add_test(NAME host_owned_example COMMAND host_owned_example $<TARGET_FILE:example_sdk> host.log)
set_tests_properties(host_owned_example PROPERTIES LABELS "integration;host-owned" TIMEOUT 15)

logger_regression(host_ownership_regression test_host_ownership.c)
target_include_directories(host_ownership_regression PRIVATE examples/host_owned)
target_link_libraries(host_ownership_regression PRIVATE ${CMAKE_DL_LIBS})
target_link_options(host_ownership_regression PRIVATE -Wl,--wrap=logger_format_line)
add_dependencies(host_ownership_regression example_sdk)
foreach(s quiet callback-only invalid-options shared-sync shared-async separate concurrent unload-pending message-data global-independent)
  add_test(NAME host_ownership_${s} COMMAND host_ownership_regression ${s} $<TARGET_FILE:example_sdk>)
  set_tests_properties(host_ownership_${s} PROPERTIES LABELS "regression;integration;host-owned" TIMEOUT 15)
endforeach()
logger_regression(source_ownership_regression test_source_ownership.c)
target_link_options(source_ownership_regression PRIVATE -Wl,--wrap=logger_format_line)
foreach(s queue-copy bounds null-source log log-source)
  add_test(NAME source_ownership_${s} COMMAND source_ownership_regression ${s})
  set_tests_properties(source_ownership_${s} PROPERTIES LABELS "regression;concurrency;host-owned" TIMEOUT 15)
endforeach()
logger_regression(destroy_status_regression test_destroy_status.c)
target_link_options(destroy_status_regression PRIVATE -Wl,--wrap=close)
foreach(s null drain io-error close-error)
  add_test(NAME destroy_status_${s} COMMAND destroy_status_regression ${s})
  set_tests_properties(destroy_status_${s} PROPERTIES LABELS "regression;reliability;host-owned" TIMEOUT 15)
endforeach()
logger_regression(host_format_regression test_host_format.c)
add_test(NAME host_format_bounds COMMAND host_format_regression)
set_tests_properties(host_format_bounds PROPERTIES LABELS "regression;unit;host-owned" TIMEOUT 15)

if(BUILD_SHARED_LIBS)
  # Real shared calls, without --wrap. Does not statically link the Logger.
  add_executable(liblogger_dlclose_test tests/test_liblogger_dlclose.c)
  target_include_directories(liblogger_dlclose_test PRIVATE include)
  target_link_libraries(liblogger_dlclose_test PRIVATE ${CMAKE_DL_LIBS})
  target_compile_options(liblogger_dlclose_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_dependencies(liblogger_dlclose_test logger)
  add_test(NAME liblogger_dlclose COMMAND liblogger_dlclose_test $<TARGET_FILE:logger>)
  set_tests_properties(liblogger_dlclose PROPERTIES LABELS "integration;shared;host-owned" TIMEOUT 20)
endif()

# File backend owner, no-clobber rotation and bound-directory regressions.
logger_regression(file_backend_regression test_file_backend.c)
target_link_options(file_backend_regression PRIVATE
  -Wl,--wrap=openat -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=clock_gettime
  -Wl,--wrap=logger_file_rename_noreplace -Wl,--wrap=unlinkat -Wl,--wrap=flock -Wl,--wrap=writev)
foreach(s owner-same owner-process owner-race owner-exit owner-alias owner-close-other
          path-empty path-long name-long path-directory path-fifo path-symlink path-dangling path-hardlink
          path-reserved-logger-lock path-reserved-audit-lock
          lock-symlink lock-hardlink lock-fifo device-rotate device-shared init-cleanup
          reopen-ok reopen-permission reopen-space reopen-emfile reopen-directory reopen-symlink reopen-sync reopen-dir-sync reopen-close
          collision clock-backwards collision-limit rotation-unsupported rotation-rename-error
          rotation-new-open rotation-dir-sync rotation-new-sync rotation-new-dir-sync cwd directory-rename active-replaced
          retention retention-error vectors eintr)
  add_test(NAME file_${s} COMMAND file_backend_regression ${s})
  set_tests_properties(file_${s} PROPERTIES LABELS "regression;file-backend;reliability" TIMEOUT 20)
endforeach()
logger_regression(file_platform_regression test_file_platform.c)
target_link_options(file_platform_regression PRIVATE -Wl,--wrap=syscall)
foreach(s real enosys einval eopnotsupp)
  add_test(NAME file_noreplace_${s} COMMAND file_platform_regression ${s})
  set_tests_properties(file_noreplace_${s} PROPERTIES LABELS "regression;file-backend;reliability" TIMEOUT 20)
endforeach()
logger_regression(file_audit_regression test_file_audit.c logger_test_support)
foreach(s logger-busy state-busy audit-busy cwd)
  add_test(NAME file_audit_${s} COMMAND file_audit_regression ${s})
  set_tests_properties(file_audit_${s} PROPERTIES LABELS "regression;file-backend;security" TIMEOUT 20)
endforeach()
foreach(alg sha256)
  foreach(point file_after_archive_rename file_after_archive_dirsync file_after_active_open file_after_active_dirsync)
    add_test(NAME file_crash_${alg}_${point} COMMAND file_audit_regression ${point} ${alg})
    set_tests_properties(file_crash_${alg}_${point} PROPERTIES LABELS "regression;file-backend;security;crash" TIMEOUT 20)
  endforeach()
endforeach()
logger_regression(file_global_regression test_file_global.c)
target_link_options(file_global_regression PRIVATE -Wl,--wrap=logger_destroy_status -Wl,--wrap=pthread_mutex_lock)
foreach(s repeat-no-effects teardown)
  add_test(NAME file_global_${s} COMMAND file_global_regression ${s})
  set_tests_properties(file_global_${s} PROPERTIES LABELS "regression;file-backend;concurrency" TIMEOUT 20)
endforeach()

logger_regression(file_legacy_probe test_file_legacy_probe.c)
target_link_options(file_legacy_probe PRIVATE -Wl,--wrap=clock_gettime)
foreach(s owner reopen collision cwd symlink hardlink global-repeat)
  add_test(NAME file_blackbox_${s} COMMAND file_legacy_probe ${s})
  set_tests_properties(file_blackbox_${s} PROPERTIES LABELS "regression;file-backend" TIMEOUT 20)
endforeach()

logger_regression(file_audit_close_regression test_file_audit_close.c)
target_link_options(file_audit_close_regression PRIVATE -Wl,--wrap=logger_create_reserved_file -Wl,--wrap=close)
add_test(NAME file_audit_close COMMAND file_audit_close_regression)
set_tests_properties(file_audit_close PROPERTIES LABELS "regression;file-backend;reliability" TIMEOUT 20)

# Syslog bounded nonblocking I/O and per-instance reconnect/metrics.
logger_regression(syslog_backend_regression test_syslog_backend.c)
foreach(s flags wire file-fails pressure multi-backend async-pressure reconnect deferred required-missing endpoint-type fd-churn init-cleanup isolation threads copy-path cwd default-tag max-tag long-tag tag-injection bad-facility bad-startup bad-interval empty-path long-path invalid-metrics fork-guard)
  add_test(NAME syslog_backend_${s} COMMAND syslog_backend_regression ${s})
  set_tests_properties(syslog_backend_${s} PROPERTIES LABELS "regression;syslog-backend;reliability" TIMEOUT 15)
endforeach()
if(BUILD_SHARED_LIBS)
  logger_regression(syslog_shared_regression test_syslog_backend.c logger)
  foreach(s flags wire file-fails pressure multi-backend async-pressure reconnect deferred required-missing endpoint-type fd-churn init-cleanup isolation threads copy-path cwd default-tag max-tag long-tag tag-injection bad-facility bad-startup bad-interval empty-path long-path invalid-metrics fork-guard)
    add_test(NAME syslog_shared_${s} COMMAND syslog_shared_regression ${s})
    set_tests_properties(syslog_shared_${s} PROPERTIES LABELS "regression;syslog-backend;shared-integration" TIMEOUT 15)
  endforeach()
endif()
logger_regression(syslog_fault_regression test_syslog_faults.c)
target_link_options(syslog_fault_regression PRIVATE -Wl,--wrap=socket -Wl,--wrap=connect -Wl,--wrap=send -Wl,--wrap=clock_gettime -Wl,--wrap=close)
foreach(s socket-emfile socket-enfile socket-enomem socket-eintr connect-eacces connect-eperm connect-enoent connect-refused connect-inprogress connect-eintr connect-again connect-prototype deferred-permission deferred-cooldown backwards-clock clock-init send-again send-nobufs send-enomem send-msgsize send-eintr send-pipe send-reset send-notconn send-refused send-io eintr-then-success zero-send short-send clock-disconnect close-disconnect reconnect-failure oversize max-packet)
  add_test(NAME syslog_fault_${s} COMMAND syslog_fault_regression ${s})
  set_tests_properties(syslog_fault_${s} PROPERTIES LABELS "regression;syslog-backend;fault" TIMEOUT 15)
endforeach()

logger_regression(syslog_config_regression test_syslog_config.c)
target_link_options(syslog_config_regression PRIVATE -Wl,--wrap=connect)
foreach(s prefix zero-prefix header-only unknown-header partial-tail future-tail)
  add_test(NAME syslog_config_${s} COMMAND syslog_config_regression ${s})
  set_tests_properties(syslog_config_${s} PROPERTIES LABELS "regression;syslog-backend;abi" TIMEOUT 15)
endforeach()
add_executable(logger_v1_consumer tests/compat/test_logger_v1_consumer.c)
target_link_libraries(logger_v1_consumer PRIVATE ${LOGGER_REGRESSION_LIBRARY})
target_link_options(logger_v1_consumer PRIVATE -Wl,--wrap=connect)
target_compile_options(logger_v1_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME syslog_old_header_consumer COMMAND logger_v1_consumer)
set_tests_properties(syslog_old_header_consumer PROPERTIES LABELS "regression;syslog-backend;abi" TIMEOUT 15)

logger_regression(syslog_fallback_regression test_syslog_fallback.c)
target_link_options(syslog_fallback_regression PRIVATE -Wl,--wrap=logger_format_line)
add_test(NAME syslog_fallback_pressure COMMAND syslog_fallback_regression)
set_tests_properties(syslog_fallback_pressure PROPERTIES LABELS "regression;syslog-backend;concurrency" TIMEOUT 15)

if(BUILD_SHARED_LIBS)
  add_executable(logger_v1_shared_consumer tests/compat/test_logger_v1_shared.c)
  target_link_libraries(logger_v1_shared_consumer PRIVATE logger)
  target_compile_options(logger_v1_shared_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME syslog_old_header_shared_consumer COMMAND logger_v1_shared_consumer)
  set_tests_properties(syslog_old_header_shared_consumer PROPERTIES LABELS "regression;syslog-backend;abi;shared-integration" TIMEOUT 15)
endif()

add_executable(syslog_client_example examples/syslog_client.c)
target_link_libraries(syslog_client_example PRIVATE logger)
target_compile_options(syslog_client_example PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Global-facade generation/admission/cancellation regressions, hooks only in tests.
logger_regression(global_lifecycle_regression test_global_lifecycle.c)
target_link_options(global_lifecycle_regression PRIVATE
  -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_rwlock_rdlock
  -Wl,--wrap=pthread_rwlock_wrlock -Wl,--wrap=logger_create
  -Wl,--wrap=logger_destroy_status -Wl,--wrap=logger_file_write
  -Wl,--wrap=logger_file_reopen -Wl,--wrap=dprintf -Wl,--wrap=fsync)
foreach(op write flush flush-void reopen level dropped metrics io)
  add_test(NAME global_generation_${op} COMMAND global_lifecycle_regression generation ${op})
  set_tests_properties(global_generation_${op} PROPERTIES LABELS "regression;concurrency;global-lifecycle" TIMEOUT 20)
endforeach()
foreach(s contract bootstrap-generation shutdown-generation start-gate stop-gate closed-readers stop-during-start double-stop shutdown-error shutdown-fsync bootstrap-stop child)
  add_test(NAME global_${s} COMMAND global_lifecycle_regression ${s})
  set_tests_properties(global_${s} PROPERTIES LABELS "regression;concurrency;global-lifecycle" TIMEOUT 20)
endforeach()
foreach(s bootstrap create write reopen destroy)
  add_test(NAME global_reentry_${s} COMMAND global_lifecycle_regression reentry ${s})
  set_tests_properties(global_reentry_${s} PROPERTIES LABELS "regression;concurrency;global-lifecycle" TIMEOUT 20)
endforeach()
foreach(s read control publish create stop)
  add_test(NAME global_error_${s} COMMAND global_lifecycle_regression acquire-error ${s})
  set_tests_properties(global_error_${s} PROPERTIES LABELS "regression;reliability;global-lifecycle" TIMEOUT 20)
endforeach()

logger_regression(global_cancel_regression test_global_cancel.c)
target_link_options(global_cancel_regression PRIVATE
  -Wl,--wrap=logger_create -Wl,--wrap=logger_destroy_status
  -Wl,--wrap=logger_file_write -Wl,--wrap=logger_file_reopen
  -Wl,--wrap=logger_queue_push -Wl,--wrap=logger_format_line
  -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=pthread_rwlock_rdlock -Wl,--wrap=dprintf)
foreach(s write queue flush flush-void reopen reader create create-fail shutdown bootstrap control-wait restore-policy)
  add_test(NAME global_cancel_${s} COMMAND global_cancel_regression ${s})
  set_tests_properties(global_cancel_${s} PROPERTIES LABELS "regression;concurrency;global-lifecycle;global-cancel" TIMEOUT 20)
endforeach()
logger_regression(global_stress_regression test_global_stress.c)
foreach(s init-race sync async)
  add_test(NAME global_stress_${s} COMMAND global_stress_regression ${s})
  set_tests_properties(global_stress_${s} PROPERTIES LABELS "regression;concurrency;global-lifecycle" TIMEOUT 30)
endforeach()
if(BUILD_SHARED_LIBS)
  logger_regression(global_shared_regression test_global_stress.c logger)
  target_compile_definitions(global_shared_regression PRIVATE LOGGER_PUBLIC_ABI_TEST=1)
  foreach(s init-race sync async)
    add_test(NAME global_shared_${s} COMMAND global_shared_regression ${s})
    set_tests_properties(global_shared_${s} PROPERTIES LABELS "regression;concurrency;global-lifecycle;shared-integration" TIMEOUT 30)
  endforeach()
endif()

# Explicit instance and Console cancellation/reentry. Link wrappers are test-only.
logger_regression(explicit_scope_regression test_explicit_scope.c)
target_link_options(explicit_scope_regression PRIVATE
  -Wl,--wrap=logger_file_write -Wl,--wrap=logger_file_reopen
  -Wl,--wrap=pthread_cond_signal -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_create -Wl,--wrap=close -Wl,--wrap=fflush
  -Wl,--wrap=vfprintf -Wl,--wrap=vsnprintf -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=pthread_setcancelstate -Wl,--wrap=logger_format_line)
foreach(s write source sync format queue fallback flush flush-void reopen syslog-metrics create create-fail destroy)
  add_test(NAME explicit_cancel_${s} COMMAND explicit_scope_regression cancel ${s})
  set_tests_properties(explicit_cancel_${s} PROPERTIES LABELS "regression;explicit-scope;concurrency" TIMEOUT 15)
endforeach()
foreach(s print info warn error verbose debug source)
  add_test(NAME console_cancel_${s} COMMAND explicit_scope_regression console-cancel ${s})
  set_tests_properties(console_cancel_${s} PROPERTIES LABELS "regression;explicit-scope;concurrency" TIMEOUT 15)
endforeach()
foreach(s log source sync flush flush-void reopen destroy destroy-void create level state dropped metrics io syslog context-set context-clear context-get console console-debug console-init console-level console-color console-tty global global-write audit audit-status)
  add_test(NAME explicit_reentry_${s} COMMAND explicit_scope_regression reentry write ${s})
  set_tests_properties(explicit_reentry_${s} PROPERTIES LABELS "regression;explicit-scope;concurrency" TIMEOUT 10)
endforeach()
foreach(origin worker console create format reopen destroy)
  foreach(s flush destroy create global audit console)
    add_test(NAME explicit_${origin}_reentry_${s} COMMAND explicit_scope_regression reentry ${origin} ${s})
    set_tests_properties(explicit_${origin}_reentry_${s} PROPERTIES LABELS "regression;explicit-scope;concurrency" TIMEOUT 10)
  endforeach()
endforeach()
foreach(s restore-policy create-async-reject setup-failure stdio-first-error context-alias debug-overflow invalid-level)
  add_test(NAME explicit_contract_${s} COMMAND explicit_scope_regression contract ${s})
  set_tests_properties(explicit_contract_${s} PROPERTIES LABELS "regression;explicit-scope;reliability" TIMEOUT 15)
endforeach()

logger_regression(console_host_regression test_console_host.c logger)
foreach(s cancel reentry config-snapshot)
  add_test(NAME console_host_${s} COMMAND console_host_regression ${s})
  set_tests_properties(console_host_${s} PROPERTIES LABELS "regression;explicit-scope;host-owned;concurrency" TIMEOUT 20)
endforeach()

foreach(s global create)
  add_test(NAME explicit_global-worker_reentry_${s} COMMAND explicit_scope_regression reentry global-worker ${s})
  set_tests_properties(explicit_global-worker_reentry_${s} PROPERTIES LABELS "regression;explicit-scope;concurrency" TIMEOUT 10)
endforeach()

# Builtin-only migration tests use frozen historical data, not a reference
# crypto package. Real production target, including in shared builds.
add_executable(crypto_builtin_only_regression tests/regression/test_crypto_builtin_only.c)
target_link_libraries(crypto_builtin_only_regression PRIVATE logger)
target_include_directories(crypto_builtin_only_regression PRIVATE src)
target_compile_options(crypto_builtin_only_regression PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(algorithm sha256)
  add_test(NAME crypto_builtin_${algorithm}_lifecycle
    COMMAND crypto_builtin_only_regression lifecycle ${algorithm})
  add_test(NAME crypto_builtin_${algorithm}_legacy
    COMMAND crypto_builtin_only_regression legacy ${algorithm}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/legacy_crypto/${algorithm}")
  set_tests_properties(crypto_builtin_${algorithm}_lifecycle crypto_builtin_${algorithm}_legacy
    PROPERTIES LABELS "regression;crypto;crypto-builtin-only" TIMEOUT 30)
endforeach()
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  set(LOGGER_ARTIFACT_ARGS "")
  if(LOGGER_ENABLE_LEGACY_FORK_HELPER)
    list(APPEND LOGGER_ARTIFACT_ARGS --legacy-fork)
  endif()
  add_test(NAME crypto_production_artifact_isolation
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_production_artifact.py"
      "$<TARGET_FILE:logger>" ${LOGGER_ARTIFACT_ARGS})
  set_tests_properties(crypto_production_artifact_isolation
    PROPERTIES LABELS "regression;crypto-builtin-only;production-isolation" TIMEOUT 30)
endif()

# SHA-256-only public and private-boundary rejection tests. The retired fixture
# is only inert negative data; no retired digest implementation is linked.
add_executable(sha256_only_regression tests/regression/test_sha256_only.c)
target_include_directories(sha256_only_regression PRIVATE src)
target_link_libraries(sha256_only_regression PRIVATE ${LOGGER_REGRESSION_LIBRARY})
target_compile_options(sha256_only_regression PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(s provider config verify-empty verify-missing live)
  add_test(NAME sha256_only_${s} COMMAND sha256_only_regression ${s})
  set_tests_properties(sha256_only_${s} PROPERTIES LABELS "regression;crypto;sha256-only" TIMEOUT 20)
endforeach()
foreach(s verify-history checkpoint-read checkpoint-write recover-id init-history init-missing-state init-relabel-state)
  add_test(NAME sha256_only_${s} COMMAND sha256_only_regression ${s}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/legacy_crypto/sm3")
  set_tests_properties(sha256_only_${s} PROPERTIES LABELS "regression;crypto;sha256-only" TIMEOUT 20)
endforeach()

include(cmake/LoggerPackagingTests.cmake)

get_property(LOGGER_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS LOGGER_ALL_TESTS)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-work/${test_name}")
  set_tests_properties(${test_name} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-work/${test_name}")
endforeach()
set(LOGGER_ASAN_PRELOAD "" CACHE FILEPATH "Optional libasan.so to preload for CTest")
if(LOGGER_ASAN_PRELOAD)
  set_property(TEST ${LOGGER_ALL_TESTS} APPEND PROPERTY ENVIRONMENT
    "LD_PRELOAD=${LOGGER_ASAN_PRELOAD}" "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1")
endif()
if(LOGGER_SANITIZE MATCHES "undefined")
  set_property(TEST ${LOGGER_ALL_TESTS} APPEND PROPERTY ENVIRONMENT
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()
# A clean `--target check` must compile test executables, not only liblogger.a.
get_property(LOGGER_LOCAL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(test_target IN LISTS LOGGER_LOCAL_TARGETS)
  get_target_property(target_type ${test_target} TYPE)
  if(target_type STREQUAL "EXECUTABLE")
    add_dependencies(check ${test_target})
    add_dependencies(check-fast ${test_target})
    add_dependencies(check-production ${test_target})
  endif()
endforeach()

endif() # BUILD_TESTING
